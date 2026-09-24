#include "prism/ipc/channel.hpp"
#include "prism/core/logging.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>

using namespace prism;

void RunBenchmark() {
    const std::string channel_name = "/prism_bench_test";
    const int TEST_PACKETS = 1000000; // 1 Million packets

    std::cout << "=========================================================\n";
    std::cout << "       Prism IPC Zero-Copy Shm Benchmark (1,000,000 Pkts) \n";
    std::cout << "=========================================================\n";

    auto host = ipc::Channel::CreateHost(channel_name);
    if (!host) {
        std::cerr << "Failed to create host channel\n";
        return;
    }

    auto client = ipc::Channel::ConnectClient(channel_name);
    if (!client) {
        std::cerr << "Failed to connect client channel\n";
        return;
    }

    // 1. Throughput Test: Client pushes 1M state diffs, Host pops
    auto start_time = std::chrono::high_resolution_clock::now();

    std::thread producer([&]() {
        for (int i = 0; i < TEST_PACKETS; ++i) {
            auto pkt = ipc::StateDiffPacket::MakeInt(core::HashSlot("progress"), i);
            while (!client->PushStateDiff(pkt)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&]() {
        int received = 0;
        ipc::StateDiffPacket pkt;
        while (received < TEST_PACKETS) {
            if (host->PopStateDiff(pkt)) {
                received++;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    double duration_sec = std::chrono::duration<double>(end_time - start_time).count();
    double qps = TEST_PACKETS / duration_sec;

    std::cout << "-> Processed: " << TEST_PACKETS << " packets in " << duration_sec << " seconds\n";
    std::cout << "-> Throughput: " << static_cast<uint64_t>(qps) << " pkts/sec (" << (qps / 1000000.0) << " Million msg/s)\n";

    // 2. Round-trip Ping-Pong Latency Test
    const int PING_COUNT = 100000;
    std::vector<double> latencies_us;
    latencies_us.reserve(PING_COUNT);

    std::cout << "\n-> Measuring Round-Trip Latency (" << PING_COUNT << " Ping-Pongs)...\n";

    std::thread ping_server([&]() {
        ipc::StateDiffPacket diff;
        for (int i = 0; i < PING_COUNT; ++i) {
            while (!host->PopStateDiff(diff)) {
                std::this_thread::yield();
            }
            ipc::EventPacket ev = ipc::EventPacket::MakeAction("echo");
            ev.timestamp_us = diff.timestamp_us;
            while (!host->PushEvent(ev)) {
                std::this_thread::yield();
            }
        }
    });

    for (int i = 0; i < PING_COUNT; ++i) {
        auto pkt = ipc::StateDiffPacket::MakeInt(1, i);
        client->PushStateDiff(pkt);

        ipc::EventPacket ev;
        while (!client->PopEvent(ev)) {
            std::this_thread::yield();
        }

        uint64_t now_us = core::CurrentTimeUs();
        double rtt_us = static_cast<double>(now_us - ev.timestamp_us);
        latencies_us.push_back(rtt_us);
    }

    ping_server.join();

    double total_lat = 0;
    for (double lat : latencies_us) total_lat += lat;
    double avg_lat_us = total_lat / PING_COUNT;

    std::cout << "-> Average Round-Trip Latency: " << avg_lat_us << " microseconds (us)\n";
    std::cout << "=========================================================\n";
}

int main() {
    RunBenchmark();
    return 0;
}
