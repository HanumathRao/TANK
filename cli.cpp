#include "tank_client.h"
#include <date.h>
#include <fcntl.h>
#include <network.h>
#include <set>
#include <sys/stat.h>
#include <sys/types.h>
#include <sysexits.h>
#include <text.h>
#include <unordered_map>

static uint64_t parse_timestamp(strwlen32_t s)
{
        strwlen32_t c;
        struct tm tm;

        // for now, YYYYMMDDHH:MM:SS
        // Eventually, will support more date/timeformats
        if (s.len != 16)
                return 0;

        c = s.Prefix(4);
        s.StripPrefix(4);
        tm.tm_year = c.AsUint32() - 1900;
        c = s.Prefix(2);
        s.StripPrefix(2);
        tm.tm_mon = c.AsUint32() - 1;
        c = s.Prefix(2);
        s.StripPrefix(2);
        tm.tm_mday = c.AsUint32();

        c = s.Prefix(2);
        s.StripPrefix(2);

        s.StripPrefix(1);
        tm.tm_hour = c.AsUint32();
        c = s.Prefix(2);
        s.StripPrefix(2);
        tm.tm_min = c.AsUint32();

        s.StripPrefix(1);
        c = s.Prefix(2);
        s.StripPrefix(2);
        tm.tm_sec = c.AsUint32();

        tm.tm_isdst = -1;

        const auto res = mktime(&tm);

        if (res == -1)
                return 0;
        else
                return Timings::Seconds::ToMillis(res);
}

int main(int argc, char *argv[])
{
        Buffer topic, endpoint;
        uint16_t partition{0};
        int r;
        TankClient tankClient;
        const char *const app = argv[0];
        bool verbose{false}, retry{false};

        if (argc == 1)
                goto help;

        tankClient.set_retry_strategy(TankClient::RetryStrategy::RetryNever);
        while ((r = getopt(argc, argv, "+vb:t:p:hrS:R:")) != -1) // see GETOPT(3) for '+' initial character semantics
        {
                switch (r)
                {
                        case 'S':
                                tankClient.set_sock_sndbuf_size(strwlen32_t(optarg).AsUint32());
                                break;

                        case 'R':
                                tankClient.set_sock_rcvbuf_size(strwlen32_t(optarg).AsUint32());
                                break;

                        case 'r':
                                retry = true;
                                tankClient.set_retry_strategy(TankClient::RetryStrategy::RetryAlways);
                                break;

                        case 'v':
                                verbose = true;
                                break;

                        case 'b':
                                endpoint.clear();
                                endpoint.append(optarg);
                                break;

                        case 't':
                                topic.clear();
                                topic.append(optarg);
                                if (topic.length() > 255)
                                {
                                        Print("Inalid topic name '", topic, "'\n");
                                        return 1;
                                }
                                break;

                        case 'p':
                        {
                                const strwlen32_t s(optarg);

                                if (!s.IsDigits())
                                {
                                        Print("Invalid partition '", s, "'. Expected numeric id from 0 upto ", UINT16_MAX, "\n");
                                        return 1;
                                }

                                const auto v = s.AsInt32();

                                if (v < 0 || v > UINT16_MAX)
                                {
                                        Print("Invalid partition\n");
                                        return 1;
                                }

                                partition = v;
                        }
                        break;

                        case 'h':
                        help:
                                Print(app, " [common options] command [command options] [command arguments]\n");
                                Print("Common options include:\n");
                                Print("-b broker endpoint: The endpoint of the Tank broker\n");
                                Print("-t topic: The topic to produce to or consume from\n");
                                Print("-p partition: The partition of the topic to produce to or consme from\n");
                                Print("-S bytes: set tank client's socket send buffer size\n");
                                Print("-R bytes: set tank client's socket receive buffer size\n");
                                Print("-v : Verbose output\n");
                                Print("Commands available: consume, produce, benchmark, discover_partitions, mirror\n");
                                return 0;

                        default:
                                Print("Please use ", app, " -h for options\n");
                                return 1;
                }
        }

        if (!topic.length())
        {
                Print("Topic not specified. Use -t to specify topic\n");
                return 1;
        }

        if (!endpoint.length())
        {
                Print("Broker endpoint not specified. Use -b to specify endpoint\n");
                return 1;
        }

        argc -= optind;
        argv += optind;

        try
        {
                tankClient.set_default_leader(endpoint.AsS32());
        }
        catch (...)
        {
                Print("Invalid broker endpoint specified '", endpoint, "'\n");
                return 1;
        }

        if (!argc)
        {
                Print("Command not specified. Please use ", app, " -h for available commands\n");
                return 1;
        }

        const strwlen32_t cmd(argv[0]);
        const TankClient::topic_partition topicPartition(topic.AsS8(), partition);
        const auto consider_fault = [](const TankClient::fault &f) {
                switch (f.type)
                {
                        case TankClient::fault::Type::BoundaryCheck:
                                Print("Boundary Check fault. first available sequence number is ", f.ctx.firstAvailSeqNum, ", high watermark is ", f.ctx.highWaterMark, "\n");
                                break;

                        case TankClient::fault::Type::UnknownTopic:
                                Print("Unknown topic '", f.topic, "' error\n");
                                break;

                        case TankClient::fault::Type::UnknownPartition:
                                Print("Unknown partition of '", f.topic, "' error\n");
                                break;

                        case TankClient::fault::Type::Access:
                                Print("Access error\n");
                                break;

			case TankClient::fault::Type::SystemFail:
				Print("System Error\n");
				break;

			case TankClient::fault::Type::InvalidReq:
				Print("Invalid Request\n");
				break;

                        case TankClient::fault::Type::Network:
                                Print("Network error\n");
                                break;

                        default:
                                break;
                }
        };

        if (cmd.Eq(_S("get")) || cmd.Eq(_S("consume")))
        {
                uint64_t next{0};
                enum class Fields : uint8_t
                {
                        SeqNum = 0,
                        Key,
                        Content,
                        TS
                };
                uint8_t displayFields{1u << uint8_t(Fields::Content)};
                static constexpr size_t defaultMinFetchSize{128 * 1024 * 1024};
                size_t minFetchSize{defaultMinFetchSize};
                uint32_t pendingResp{0};
                bool statsOnly{false};
                IOBuffer buf;
                range64_t timeRange{0, UINT64_MAX};

                optind = 0;
                while ((r = getopt(argc, argv, "+SF:hBT:")) != -1)
                {
                        switch (r)
                        {
                                case 'T':
                                {
                                        const auto r = strwlen32_t(optarg).Divided(',');

                                        timeRange.offset = parse_timestamp(r.first);
                                        if (!timeRange.offset)
                                        {
                                                Print("Failed to parse ", r.first, "\n");
                                                return 1;
                                        }

                                        if (r.second)
                                        {
                                                const auto end = parse_timestamp(r.second);

                                                if (!end)
                                                {
                                                        Print("Failed to parse ", r.second, "\n");
                                                        return 1;
                                                }
                                                else
                                                        timeRange.SetEnd(end + 1);
                                        }
                                        else
                                                timeRange.len = UINT64_MAX - timeRange.offset;
                                }
                                break;

                                case 'S':
                                        statsOnly = true;
                                        break;

                                case 'F':
                                        displayFields = 0;
                                        for (const auto it : strwlen32_t(optarg).Split(','))
                                        {
                                                if (it.Eq(_S("seqnum")))
                                                        displayFields |= 1u << uint8_t(Fields::SeqNum);
                                                else if (it.Eq(_S("key")))
                                                        displayFields |= 1u << uint8_t(Fields::Key);
                                                else if (it.Eq(_S("content")))
                                                        displayFields |= 1u << uint8_t(Fields::Content);
                                                else if (it.Eq(_S("ts")))
                                                        displayFields |= 1u << uint8_t(Fields::TS);
                                                else
                                                {
                                                        Print("Unknown field '", it, "'\n");
                                                        return 1;
                                                }
                                        }
                                        break;

                                case 'h':
                                        Print("CONSUME [options] from\n");
                                        Print("Options include:\n");
                                        Print("-F display format: Specify a ',' separated list of message properties to be displayed. Properties include: \"seqnum\", \"key\", \"content\", \"ts\". By default, only the content is displayed\n");
                                        Print("-S: statistics only\n");
                                        Print("-T: optionally, filter all consumes messages by specifying a time range in either (from,to) or (from) format, where the first allows to specify a start and an end date/time and the later a start time and no end time. Currently, only one date-time format is supported (YYYMMDDHH:MM:SS)\n");
                                        Print("\"from\" specifies the first message we are interested in.\n");
                                        Print("If from is \"beginning\" or \"start\","
                                              " it will start consuming from the first available message in the selected topic. If it is \"eof\" or \"end\", it will "
                                              "tail the topic for newly produced messages, otherwise it must be an absolute 64bit sequence number\n");
                                        return 0;

                                default:
                                        return 1;
                        }
                }

                argc -= optind;
                argv += optind;

                if (!argc)
                {
                        Print("Expected sequence number to begin consuming from. Please see ", app, " consume -h\n");
                        return 1;
                }
                else
                {
                        const strwlen32_t from(argv[0]);

                        if (from.EqNoCase(_S("beginning")) || from.Eq(_S("first")))
                                next = 0;
                        else if (from.EqNoCase(_S("end")) || from.EqNoCase(_S("eof")))
                                next = UINT64_MAX;
                        else if (!from.IsDigits())
                        {
                                Print("Expected either \"beginning\", \"end\" or a sequence number for -f option\n");
                                return 1;
                        }
                        else
                                next = from.AsUint64();
                }

                for (;;)
                {
                        if (!pendingResp)
                        {
                                if (verbose)
                                        Print("Requesting from ", next, "\n");

                                pendingResp = tankClient.consume({{topicPartition, {next, minFetchSize}}}, 8e3, 0);

                                if (!pendingResp)
                                {
                                        Print("Unable to issue consume request. Will abort\n");
                                        return 1;
                                }
                        }

                        try
                        {
                                tankClient.poll(1e3);
                        }
                        catch (...)
                        {
                                continue;
                        }

                        for (const auto &it : tankClient.faults())
                        {
                                consider_fault(it);
                                if (retry && it.type == TankClient::fault::Type::Network)
                                {
                                        Timings::Milliseconds::Sleep(400);
                                        pendingResp = 0;
                                }
                                else
                                        return 1;
                        }

                        for (const auto &it : tankClient.consumed())
                        {
                                if (statsOnly)
                                {
                                        Print(it.msgs.len, " messages\n");
                                }
                                else
                                {
                                        size_t sum{0};
                                        for (const auto m : it.msgs)
                                                sum += m->content.len;
                                        sum += it.msgs.len * 2;

                                        buf.clear();
                                        buf.reserve(sum);
                                        for (const auto m : it.msgs)
                                        {
                                                if (timeRange.Contains(m->ts))
                                                {
                                                        buf.append(m->content);
                                                        buf.append('\n');
                                                }
                                        }

                                        if (auto s = buf.AsS32())
                                        {
                                                do
                                                {
                                                        const auto r = write(STDOUT_FILENO, s.p, s.len);

                                                        if (r == -1)
                                                        {
                                                                Print("(Failed to output data to stdout:", strerror(errno), ". Exiting\n");
                                                                return 1;
                                                        }
                                                        else
                                                                s.StripPrefix(r);
                                                } while (s);
                                        }
                                }

                                minFetchSize = Max<size_t>(it.next.minFetchSize, defaultMinFetchSize);
                                next = it.next.seqNum;
                                pendingResp = 0;
                        }
                }
        }
        else if (cmd.Eq(_S("mirror")))
        {
                TankClient dest;
                uint32_t reqId1, reqId2;

                // TODO: throttling options
                optind = 0;
                while ((r = getopt(argc, argv, "+h")) != -1)
                {
                        switch (r)
                        {
                                case 'h':
                                        Print("mirror [options] endpoint\n");
                                        Print("Will mirror the selected topic's partitions to the broker identified by <endpoint>.\n");
                                        Print("You should have created the partitions(directories) in the destination before you attempt to mirror from source to destination.\n");
                                        return 0;

                                default:
                                        return 1;
                        }
                }

                argc -= optind;
                argv += optind;

                if (!argc)
                {
                        Print("Mirror destination endpoint not specified\n");
                        return 1;
                }

                try
                {
                        dest.set_default_leader(strwlen32_t(argv[0]));
                }
                catch (...)
                {
                        Print("Invalid destination endpoint\n");
                        return 1;
                }

                // Discover partitions first
                reqId1 = tankClient.discover_partitions(topicPartition.first);
                if (!reqId1)
                {
                        Print("Unable to schedule discover request to source\n");
                        return 1;
                }

                reqId2 = dest.discover_partitions(topicPartition.first);
                if (!reqId2)
                {
                        Print("Unable to schedule discover request to destination\n");
                        return 1;
                }

                struct partition_ctx
                {
                        uint16_t id;
                        bool pending;
                        uint64_t next;
                };

                simple_allocator allocator{4096};
                uint16_t srcPartitionsCnt{0};
                std::unordered_map<uint16_t, partition_ctx *> map;
                std::vector<partition_ctx *> pending;
                std::vector<std::pair<TankClient::topic_partition, std::pair<uint64_t, uint32_t>>> inputs;
                std::vector<std::pair<TankClient::topic_partition, std::vector<TankClient::msg>>> outputs;

                while (tankClient.should_poll())
                {
                        tankClient.poll(1e3);

                        for (const auto &it : tankClient.faults())
                        {
                                consider_fault(it);
                                return 1;
                        }

                        if (tankClient.discovered_partitions().size())
                        {
                                const auto &v = tankClient.discovered_partitions().front();

                                require(v.clientReqId == reqId1);
                                srcPartitionsCnt = v.watermarks.len;
                        }
                }

                while (dest.should_poll())
                {
                        dest.poll(1e3);

                        for (const auto &it : dest.faults())
                        {
                                consider_fault(it);
                                return 1;
                        }

                        if (dest.discovered_partitions().size())
                        {
                                const auto &v = dest.discovered_partitions().front();

                                require(v.clientReqId == reqId2);
                                pending.reserve(v.watermarks.len);
                                for (const auto p : v.watermarks)
                                {
                                        auto partition = allocator.Alloc<partition_ctx>();

                                        partition->id = pending.size();
                                        partition->pending = true;
                                        partition->next = p->second + 1;
                                        pending.push_back(partition);
                                        map.insert({partition->id, partition});
                                }
                        }
                }

                if (pending.empty())
                {
                        Print("No partitions discovered - nothing to mirror\n");
                        return 1;
                }
                else if (srcPartitionsCnt != pending.size())
                {
                        Print("Partitions mismatch, ", dotnotation_repr(srcPartitionsCnt), " partitions discovered in source, ", dotnotation_repr(pending.size()), " in destination\n");
                        return 1;
                }



                Print("Will now mirror ", dotnotation_repr(srcPartitionsCnt), " partitions of ", ansifmt::bold, topicPartition.first, ansifmt::reset, "\n");
                Print("You can safely abort mirroring by stoping this tank-cli process (e.g CTRL-C or otherwise). Next mirror session will pick up mirroring from where this session ended\n");

                for (reqId1 = 0;;)
                {
                        if (!reqId1 && pending.size() && !dest.should_poll())
                        {
                                inputs.clear();
                                for (const auto it : pending)
                                {
                                        if (verbose)
                                                Print("Scheduling for partition ", it->id, " from ", it->next, "\n");

                                        it->pending = false;
                                        inputs.push_back({{topicPartition.first, it->id}, {it->next, 4 * 1024 * 1024}});
                                }

                                reqId1 = tankClient.consume(inputs, 4e3, 0);
                                if (!reqId1)
                                {
                                        Print("Failed to issue consume request\n");
                                        return 1;
                                }

                                pending.clear();
                        }

                        if (tankClient.should_poll())
                        {
                                tankClient.poll(1e2);

                                if (tankClient.faults().size())
                                {
                                        for (const auto &it : tankClient.faults())
                                                consider_fault(it);

                                        return 1;
                                }

                                if (tankClient.consumed().size())
                                {
                                        outputs.clear();

                                        for (const auto &it : tankClient.consumed())
                                        {
                                                auto partition = map[it.partition];

                                                if (it.msgs.len)
                                                {
                                                        std::vector<TankClient::msg> msgs;
                                                        size_t sum{0};

                                                        msgs.reserve(it.msgs.len);
                                                        for (const auto m : it.msgs)
                                                        {
                                                                if (msgs.size() == 256 || sum > 4 * 1024 * 1024) // XXX: arbitrary
                                                                {
                                                                        outputs.push_back({{topicPartition.first, it.partition}, std::move(msgs)});
                                                                        sum = 0;
                                                                        msgs.clear();
                                                                }

                                                                sum += m->key.len + m->content.len + 32;
                                                                msgs.push_back({m->content, m->ts, m->key});
                                                        }

                                                        outputs.push_back({{topicPartition.first, it.partition}, std::move(msgs)});
                                                }
                                                else
                                                {
                                                        // could have been if timed out
                                                        if (!partition->pending)
                                                        {
                                                                partition->pending = true;
                                                                pending.push_back(partition);
                                                        }
                                                }

                                                partition->next = it.next.seqNum;
                                        }

                                        if (outputs.size())
                                        {
                                                reqId2 = dest.produce(outputs);

                                                if (!reqId2)
                                                {
                                                        Print("Failed to schedule product request to destination\n");
                                                        return 1;
                                                }
                                        }

                                        reqId1 = 0;
                                }
                        }

                        if (dest.should_poll())
                        {
                                dest.poll(1e2);

                                if (dest.faults().size())
                                {
                                        for (const auto &it : dest.faults())
                                                consider_fault(it);

                                        return 1;
                                }

                                for (const auto &it : dest.produce_acks())
                                {
                                        const auto partition = it.partition;
                                        auto p = map[partition];

                                        if (!p->pending)
                                        {
                                                p->pending = true;
                                                pending.push_back(p);
                                        }
                                }
                        }
                }

                return 0;
        }
        else if (cmd.Eq(_S("discover_partitions")))
        {
                const auto reqId = tankClient.discover_partitions(topicPartition.first);

                if (!reqId)
                {
                        Print("Unable to schedule discover partition request\n");
                        return 1;
                }

                while (tankClient.should_poll())
                {
                        tankClient.poll(1e3);

                        for (const auto &it : tankClient.faults())
                                consider_fault(it);

                        for (const auto &it : tankClient.discovered_partitions())
                        {
                                uint16_t i{0};

                                require(it.clientReqId == reqId);
                                Print(dotnotation_repr(it.watermarks.len), " partitions for '", topicPartition.first, "'\n");

                                // http://www.tldp.org/HOWTO/Bash-Prompt-HOWTO/x361.html
                                Print(ansifmt::bold, "Partition\r\033\[<10CFirst Available\r\033\[<30CLast Assigned", ansifmt::reset, "\n");
                                for (const auto it : it.watermarks)
                                        Print(ansifmt::bold, i++, ansifmt::reset, "\033\[<10C", it->first, "\r\033[<30C", it->second, "\n");
                        }
                }

                return 0;
        }
        else if (cmd.Eq(_S("set")) || cmd.Eq(_S("produce")) || cmd.Eq(_S("publish")))
        {
                char path[PATH_MAX];
                size_t bundleSize{1};
                bool asSingleMsg{false};
                uint64_t baseSeqNum{0};

                optind = 0;
                path[0] = '\0';
                while ((r = getopt(argc, argv, "+s:f:F:hS:")) != -1)
                {
                        switch (r)
                        {
                                case 'S':
                                        baseSeqNum = strwlen32_t(optarg).AsUint64();
                                        break;

                                case 's':
                                        bundleSize = strwlen32_t(optarg).AsUint32();
                                        if (!bundleSize)
                                        {
                                                Print("Invalid bundle size specified\n");
                                                return 1;
                                        }
                                        break;

                                case 'F':
                                        asSingleMsg = true;
                                case 'f':
                                {
                                        const auto l = strlen(optarg);

                                        require(l < sizeof(path));
                                        memcpy(path, optarg, l);
                                        path[l] = '\0';
                                }
                                break;

                                case 'h':
                                        Print("PRODUCE options [message1 message2...]\n");
                                        Print("Options include:\n");
                                        Print("-s number: The bundle size; how many messages to be grouped into a bundle before producing that to the broker. Default is 1, which means each new message is published as a single bundle\n");
                                        Print("-f file: The messages are read from `file`, which is expected to contain the mesasges in every new line. The `file` can be \"-\" for stdin. If this option is provided, the messages list is ignored\n");
                                        Print("-F file: Like '-f file', except that the contents of the file will be stored as a single message\n");
                                        return 1;

                                default:
                                        return 1;
                        }
                }

                argc -= optind;
                argv += optind;

                static constexpr size_t pollInterval{20};
                std::vector<TankClient::msg> msgs;
                size_t pendingAckAcnt{0};
                std::set<uint32_t> pendingResps;
                const auto poll = [&]() {
                        tankClient.poll(8e2);

                        for (const auto &it : tankClient.faults())
                        {
                                consider_fault(it);
                                if (retry && it.type == TankClient::fault::Type::Network)
                                {
                                        Timings::Milliseconds::Sleep(400);
                                        pendingResps.clear();
                                }
                                else
                                        return false;
                        }

                        for (const auto &it : tankClient.produce_acks())
                                pendingResps.erase(it.clientReqId);

                        return true;
                };
                const auto publish_msgs = [&]() {
                        uint32_t reqId;

                        if (verbose)
                                Print("Publishing ", msgs.size(), " messages\n");

                        if (baseSeqNum)
                        {
                                reqId = tankClient.produce_with_base({{topicPartition, {baseSeqNum, msgs}}});
                                baseSeqNum = 0;
                        }
                        else
                        {
                                reqId = tankClient.produce({{topicPartition, msgs}});
                        }

                        if (!reqId)
                        {
                                Print("Failed to schedule messages to broker\n");
                                return false;
                        }
                        else
                                pendingResps.insert(reqId);

                        pendingAckAcnt += msgs.size();

                        if (pendingAckAcnt >= pollInterval)
                        {
                                while (tankClient.should_poll())
                                {
                                        if (!poll())
                                                return false;
                                }

                                pendingAckAcnt = 0;
                        }

                        return true;
                };

                if (path[0])
                {
                        static constexpr size_t bufSize{64 * 1024};
                        int fd;
                        auto *const buf = (char *)malloc(bufSize);
                        range32_t range;
                        strwlen32_t s;

                        Defer({
                                free(buf);
                                if (fd != STDIN_FILENO && fd != -1)
                                        close(fd);
                        });

                        if (!strcmp(path, "-"))
                        {
                                fd = STDIN_FILENO;
                                require(fd != -1);
                        }
                        else
                        {
                                fd = open(path, O_RDONLY | O_LARGEFILE);

                                if (fd == -1)
                                {
                                        Print("Failed to open(", path, "): ", strerror(errno), "\n");
                                        return 1;
                                }
                        }

                        const auto consider_input = [&](const bool last) {

                                while (range)
                                {
                                        s.Set(buf + range.offset, range.len);

                                        if (const auto *const p = s.Search('\n'))
                                        {
                                                const uint32_t n = p - s.p;
                                                auto data = (char *)malloc(n);

                                                memcpy(data, s.p, n);
                                                msgs.push_back({{data, n}, Timings::Milliseconds::SysTime(), {}});
                                                range.TrimLeft(n + 1);

                                                if (msgs.size() == bundleSize)
                                                {
                                                        const auto r = publish_msgs();

                                                        for (auto &it : msgs)
                                                                std::free(const_cast<char *>(it.content.p));

                                                        msgs.clear();

                                                        if (!r)
                                                                return false;
                                                }
                                        }
                                        else
                                                break;
                                }

                                if (last && msgs.size())
                                {
                                        const auto r = publish_msgs();

                                        for (auto &it : msgs)
                                                std::free(const_cast<char *>(it.content.p));

                                        msgs.clear();

                                        if (!r)
                                                return false;
                                }

                                if (!range)
                                        range.Unset();

                                return true;
                        };

                        range.Unset();

                        if (asSingleMsg)
                        {
                                IOBuffer content;

                                for (;;)
                                {
                                        if (content.Capacity() < 8192)
                                                content.reserve(65536);

                                        const auto r = read(fd, content.End(), content.Capacity());

                                        if (r == -1)
                                        {
                                                Print("Failed to read data:", strerror(errno), "\n");
                                                return 1;
                                        }
                                        else if (!r)
                                                break;
                                        else
                                                content.AdvanceLength(r);
                                }

                                if (verbose)
                                        Print("Publishing message of size ", size_repr(content.length()), "\n");

                                msgs.push_back({content.AsS32(), Timings::Milliseconds::SysTime(), {}});

                                const auto reqId = tankClient.produce(
                                    {{topicPartition, msgs}});

                                if (!reqId)
                                {
                                        Print("Failed to schedule messages to broker\n");
                                        return false;
                                }
                                else
                                        pendingResps.insert(reqId);
                        }
                        else
                        {
                                for (;;)
                                {
                                        const auto capacity = bufSize - range.End();
                                        auto r = read(fd, buf + range.offset, capacity);

                                        if (r == -1)
                                        {
                                                Print("Failed to read data:", strerror(errno), "\n");
                                                return 1;
                                        }
                                        else if (!r)
                                                break;
                                        else
                                        {
                                                range.len += r;
                                                if (!consider_input(false))
                                                        return false;
                                        }
                                }

                                if (!consider_input(true))
                                        return false;
                        }

                        while (tankClient.should_poll())
                        {
                                if (!poll())
                                        return 1;
                        }
                }
                else if (!argc)
                {
                        Print("No messages specified, and no input file was specified with -f. Please see ", app, " produce -h\n");
                        return 1;
                }
                else
                {
                        for (uint32_t i{0}; i != argc; ++i)
                        {
                                msgs.push_back({strwlen32_t(argv[i]), Timings::Milliseconds::SysTime(), {}});

                                if (msgs.size() == bundleSize)
                                {
                                        if (!publish_msgs())
                                                return false;
                                        msgs.clear();
                                }
                        }

                        if (msgs.size())
                        {
                                if (!publish_msgs())
                                        return false;

                                msgs.clear();
                        }

                        while (tankClient.should_poll())
                        {
                                if (!poll())
                                        return false;
                        }
                }
        }
        else if (cmd.Eq(_S("benchmark")) || cmd.Eq(_S("bm")))
        {
                optind = 0;
                while ((r = getopt(argc, argv, "+h")) != -1)
                {
                        switch (r)
                        {

                                case 'h':
                                        Print("BENCHMARK [options] type [options]\n");
                                        Print("Type can be:\n");
                                        Print("p2c:  Measures latency when producing from client to broker and consuming(tailing) the broker that message\n");
                                        Print("p2b:  Measures latency when producing from client to broker\n");
                                        Print("Options include:\n");
                                        return 0;

                                default:
                                        return 1;
                        }
                }
                argc -= optind;
                argv += optind;

                if (!argc)
                {
                        Print("Benchmark type not selected. Please use -h option for more\n");
                        return 1;
                }

                const strwlen32_t type(argv[0]);

                if (type.Eq(_S("p2c")))
                {
                        // Measure latency when publishing from publisher to broker and from broker to consume
                        // Submit messages to the broker and wait until you get them back
                        size_t size{128}, cnt{1};

                        optind = 0;
                        while ((r = getopt(argc, argv, "+hc:s:R")) != -1)
                        {
                                switch (r)
                                {
                                        case 'R':
                                                tankClient.set_compression_strategy(TankClient::CompressionStrategy::CompressNever);
                                                break;

                                        case 'c':
                                                cnt = strwlen32_t(optarg).AsUint32();
                                                break;

                                        case 's':
                                                size = strwlen32_t(optarg).AsUint32();
                                                break;

                                        case 'h':
                                                Print("Performs a produce to consumer via Tank latency test. It will produce messages while also 'tailing' the selected topic and will measure how long it takes for the messages to reach the broker, stored, forwarded to the client and received\n");
                                                Print("Options include:\n");
                                                Print("-s message content length: by default 11bytes\n");
                                                Print("-c total messages to publish: by default 1 message\n");
                                                Print("-R: do not compress bundle\n");
                                                return 0;

                                        default:
                                                return 1;
                                }
                        }
                        argc -= optind;
                        argv += optind;

                        auto *p = (char *)malloc(size + 16);

                        memset(p, 0, size);

                        const strwlen32_t content(p, size);
                        std::vector<TankClient::msg> msgs;

                        if (tankClient.consume({{topicPartition, {UINT64_MAX, 1e4}}}, 10e3, 0) == 0)
                        {
                                Print("Unable to schedule consumer request\n");
                                return 1;
                        }

                        Defer({ free(p); });

                        msgs.reserve(cnt);

                        for (uint32_t i{0}; i != cnt; ++i)
                                msgs.push_back({content, 0, {}});

                        const auto start{Timings::Microseconds::Tick()};

                        if (tankClient.produce(
                                {{
                                    topicPartition, msgs,
                                }}) == 0)
                        {
                                Print("Unable to schedule publisher request\n");
                                return 1;
                        }

                        while (tankClient.should_poll())
                        {
                                tankClient.poll(1e3);

                                for (const auto &it : tankClient.faults())
                                {
                                        consider_fault(it);
                                        return false;
                                }

                                if (tankClient.consumed().size())
                                {
                                        Print("Go data after publishing ", dotnotation_repr(cnt), " message(s) of size ", size_repr(content.len), " (", size_repr(cnt * content.len), "), took ", duration_repr(Timings::Microseconds::Since(start)), "\n");
                                        return 0;
                                }
                        }
                }
                else if (type.Eq(_S("p2t")))
                {
                        // Measure latency when publishing from publisher to broker and from broker to consume
                        // Submit messages to the broker and wait until you get them back
                        size_t size{128}, cnt{1};

                        optind = 0;
                        while ((r = getopt(argc, argv, "+hc:s:R")) != -1)
                        {
                                switch (r)
                                {
                                        case 'R':
                                                tankClient.set_compression_strategy(TankClient::CompressionStrategy::CompressNever);
                                                break;

                                        case 'c':
                                                cnt = strwlen32_t(optarg).AsUint32();
                                                break;

                                        case 's':
                                                size = strwlen32_t(optarg).AsUint32();
                                                break;

                                        case 'h':
                                                Print("Performs a produce to tank latency test. It will produce messages while also 'tailing' the selected topic and will measure how long it takes for the messages to reach the broker, stored, and acknowledged to the client\n");
                                                Print("Options include:\n");
                                                Print("-s message contrent length: by default 11bytes\n");
                                                Print("-c total messages to publish: by default 1 message\n");
                                                Print("-R: do not compress bundle\n");
                                                return 0;

                                        default:
                                                return 1;
                                }
                        }
                        argc -= optind;
                        argv += optind;

                        auto *p = (char *)malloc(size + 16);

                        memset(p, 0, size);

                        const strwlen32_t content(p, size);
                        std::vector<TankClient::msg> msgs;

                        Defer({ free(p); });

                        msgs.reserve(cnt);
                        for (uint32_t i{0}; i != cnt; ++i)
                                msgs.push_back({content, 0, {}});

                        const auto start{Timings::Microseconds::Tick()};

                        if (tankClient.produce({{
                                topicPartition, msgs,
                            }}) == 0)
                        {
                                Print("Unable to schedule publisher request\n");
                                return 1;
                        }

                        while (tankClient.should_poll())
                        {
                                tankClient.poll(1e3);

                                for (const auto &it : tankClient.faults())
                                {
                                        consider_fault(it);
                                        return false;
                                }

                                if (tankClient.produce_acks().size())
                                {
                                        Print("Go ACK after publishing ", dotnotation_repr(cnt), " message(s) of size ", size_repr(content.len), " (", size_repr(cnt * content.len), "), took ", duration_repr(Timings::Microseconds::Since(start)), "\n");
                                        return 0;
                                }
                        }
                }
                else
                {
                        Print("Unknown benchmark type\n");
                        return 1;
                }
        }
        else
        {
                Print("Command '", cmd, "' not supported. Please see ", app, " -h\n");
                return 1;
        }

        return EX_OK;
}
