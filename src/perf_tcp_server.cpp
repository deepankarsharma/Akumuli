#include <iostream>
#include <thread>

#include "tcp_server.h"
#include "perftest_tools.h"
#include <sys/time.h>

using namespace Akumuli;

struct DbMock : DbConnection {
    typedef std::tuple<aku_ParamId, aku_TimeStamp, double> ValueT;
    size_t nrec = 0u;
    PerfTimer tm;
    Logger logger_ = Logger("dbmock", 100);

    aku_Status write_double(aku_ParamId param, aku_TimeStamp ts, double data) {
        static const int N = 1000000;
        if (nrec++ % N == 0) {
            double elapsed = tm.elapsed();
            auto throughput = static_cast<int>(N*(1.0/elapsed));
            logger_.info() << "Server throughput " << throughput << " msg/sec";
            tm.restart();
        }
        return AKU_SUCCESS;
    }
};

int main(int argc, char *argv[]) {
    std::cout << "Tcp server performance test" << std::endl;
    auto con = std::make_shared<DbMock>();
    auto server = std::make_shared<TcpServer>(con, 4);
    server->start();
    server->wait();
    server->stop();
}
