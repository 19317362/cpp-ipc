#include <iostream>
#include <string>
#include <type_traits>
#include <memory>
#include <new>
#include <vector>
#include <unordered_map>

#include "circ_elem_array.h"
#include "circ_queue.h"
#include "test.h"

namespace {

using cq_t = ipc::circ::elem_array<12>;
cq_t* cq__;

struct msg_t {
    int pid_;
    int dat_;
};

bool operator==(msg_t const & m1, msg_t const & m2) {
    return (m1.pid_ == m2.pid_) && (m1.dat_ == m2.dat_);
}

} // internal-linkage

template <>
struct test_verify<cq_t> {
    std::unordered_map<int, std::vector<int>>* list_;
    int lcount_;

    test_verify(int M) {
        list_ = new std::remove_reference_t<decltype(*list_)>[
                    static_cast<std::size_t>(lcount_ = M)
                ];
    }

    ~test_verify() {
        delete [] list_;
    }

    void prepare(void* pt) {
        std::cout << "start consumer: " << pt << std::endl;
    }

    void push_data(int cid, msg_t const & msg) {
        list_[cid][msg.pid_].push_back(msg.dat_);
    }

    void verify(int N, int Loops) {
        std::cout << "verifying..." << std::endl;
        for (int m = 0; m < lcount_; ++m) {
            auto& cons_vec = list_[m];
            for (int n = 0; n < N; ++n) {
                auto& vec = cons_vec[n];
                QCOMPARE(vec.size(), static_cast<std::size_t>(Loops));
                int i = 0;
                for (int d : vec) {
                    QCOMPARE(i, d);
                    ++i;
                }
            }
        }
    }
};

template <std::size_t D>
struct test_cq<ipc::circ::elem_array<D>> {
    using ca_t = ipc::circ::elem_array<D>;
    using cn_t = typename ca_t::u2_t;

    ca_t* ca_;

    test_cq(ca_t* ca) : ca_(ca) {
        ::new (ca) ca_t;
    }

    cn_t connect() {
        auto cur = ca_->cursor();
        ca_->connect();
        return cur;
    }

    void disconnect(cn_t) {
        ca_->disconnect();
    }

    void wait_start(int M) {
        while (ca_->conn_count() != static_cast<std::size_t>(M)) {
            std::this_thread::yield();
        }
    }

    template <typename F>
    void recv(cn_t cur, F&& proc) {
        do {
            while (cur != ca_->cursor()) {
                msg_t* pmsg = static_cast<msg_t*>(ca_->take(cur)),
                        msg = *pmsg;
                ca_->put(pmsg);
                if (msg.pid_ < 0) return;
                ++cur;
                proc(msg);
            }
            std::this_thread::yield();
        } while(1);
    }

    ca_t* connect_send() {
        return ca_;
    }

    void send(ca_t* ca, msg_t const & msg) {
        msg_t* pmsg = static_cast<msg_t*>(ca->acquire());
        (*pmsg) = msg;
        ca->commit(pmsg);
    }
};

template <typename T>
struct test_cq<ipc::circ::queue<T>> {
    using cn_t = ipc::circ::queue<T>;
    using ca_t = typename cn_t::array_t;

    ca_t* ca_;

    test_cq(void*) : ca_(reinterpret_cast<ca_t*>(cq__)) {
        ::new (ca_) ca_t;
    }

    cn_t* connect() {
        cn_t* queue = new cn_t { ca_ };
        [&] { QVERIFY(queue->connect() != ipc::invalid_value); } ();
        return queue;
    }

    void disconnect(cn_t* queue) {
        QVERIFY(queue->disconnect() != ipc::invalid_value);
        QVERIFY(queue->detach() != nullptr);
        delete queue;
    }

    void wait_start(int M) {
        while (ca_->conn_count() != static_cast<std::size_t>(M)) {
            std::this_thread::yield();
        }
    }

    template <typename F>
    void recv(cn_t* queue, F&& proc) {
        do {
            auto msg = queue->pop();
            if (msg.pid_ < 0) return;
            proc(msg);
        } while(1);
    }

    cn_t connect_send() {
        return cn_t{ ca_ };
    }

    void send(cn_t& cn, msg_t const & msg) {
        cn.push(msg);
    }
};

namespace {

class Unit : public TestSuite {
    Q_OBJECT

    const char* name() const {
        return "test_circ";
    }

private slots:
    void initTestCase();
    void cleanupTestCase();

    void test_inst();
    void test_prod_cons_1v1();
    void test_prod_cons_1v3();
    void test_prod_cons_performance();
    void test_queue();
} unit__;

#include "test_circ.moc"

constexpr int LoopCount = 1000000;

void Unit::initTestCase() {
    TestSuite::initTestCase();
    cq__ = new cq_t;
}

void Unit::cleanupTestCase() {
    delete cq__;
}

void Unit::test_inst() {
    std::cout << "cq_t::head_size  = " << cq_t::head_size  << std::endl;
    std::cout << "cq_t::data_size  = " << cq_t::data_size  << std::endl;
    std::cout << "cq_t::elem_size  = " << cq_t::elem_size  << std::endl;
    std::cout << "cq_t::block_size = " << cq_t::block_size << std::endl;

    QCOMPARE(static_cast<std::size_t>(cq_t::data_size) , static_cast<std::size_t>(12));
    QCOMPARE(sizeof(cq_t), static_cast<std::size_t>(cq_t::block_size + cq_t::head_size));

    std::cout << "sizeof(ipc::circ::elem_array<4096>) = " << sizeof(*cq__) << std::endl;

    auto a = cq__->take(1);
    auto b = cq__->take(2);
    QCOMPARE(static_cast<std::size_t>(static_cast<ipc::byte_t*>(b) -
                                      static_cast<ipc::byte_t*>(a)),
             static_cast<std::size_t>(cq_t::elem_size));
}

template <int N, int M, bool V = true, int Loops = LoopCount>
void test_prod_cons() {
    benchmark_prod_cons<N, M, Loops, std::conditional_t<V, cq_t, void>>(cq__);
}

void Unit::test_prod_cons_1v1() {
    test_prod_cons<1, 1>();
}

void Unit::test_prod_cons_1v3() {
    test_prod_cons<1, 3>();
}

template <int P, int C>
struct test_performance {
    static void start() {
        test_performance<P - 1, C - 1>::start();
        test_prod_cons<P, C, false>();
    }
};

template <int C>
struct test_performance<1, C> {
    static void start() {
        test_performance<1, C - 1>::start();
        test_prod_cons<1, C, false>();
    }
};

template <int P>
struct test_performance<P, 1> {
    static void start() {
        test_performance<P - 1, 1>::start();
        test_prod_cons<P, 1, false>();
    }
};

template <>
struct test_performance<1, 1> {
    static void start() {
        test_prod_cons<1, 1, false>();
    }
};

void Unit::test_prod_cons_performance() {
    test_performance<1, 10>::start();
    test_prod_cons  <1, 10>(); // test & verify
}

void Unit::test_queue() {
    ipc::circ::queue<msg_t> queue;
    queue.push(msg_t { 1, 2 });
    QCOMPARE(queue.pop(), msg_t{});
    QVERIFY(sizeof(decltype(queue)::array_t) <= sizeof(*cq__));

    auto cq = ::new (cq__) decltype(queue)::array_t;
    queue.attach(cq);
    QVERIFY(queue.detach() != nullptr);

    benchmark_prod_cons<1, 1, LoopCount>((ipc::circ::queue<msg_t>*)nullptr);
    benchmark_prod_cons<1, 2, LoopCount>((ipc::circ::queue<msg_t>*)nullptr);
    benchmark_prod_cons<1, 4, LoopCount>((ipc::circ::queue<msg_t>*)nullptr);
    benchmark_prod_cons<1, 8, LoopCount>((ipc::circ::queue<msg_t>*)nullptr);
}

} // internal-linkage
