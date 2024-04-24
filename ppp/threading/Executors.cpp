#include <ppp/threading/Executors.h>
#include <ppp/threading/Timer.h>
#include <ppp/threading/Thread.h>
#include <common/libtcpip/netstack.h>

#if defined(_WIN32)
#include <windows/ppp/win32/Win32Native.h>
#endif

namespace ppp
{
    namespace threading
    {
        typedef std::shared_ptr<Byte>                                           BufferArray;
        typedef std::mutex                                                      SynchronizedObject;
        typedef std::lock_guard<SynchronizedObject>                             SynchronizedObjectScope;
        typedef std::shared_ptr<boost::asio::io_context>                        ExecutorContextPtr;
        typedef ppp::unordered_map<int64_t, ExecutorContextPtr>                 ExecutorTable;
        typedef ppp::list<ExecutorContextPtr>                                   ExecutorLinkedList;
        typedef std::shared_ptr<Thread>                                         ExecutorThreadPtr;
        typedef ppp::unordered_map<boost::asio::io_context*, ExecutorThreadPtr> ExecutorThreadTable;
        typedef ppp::unordered_map<boost::asio::io_context*, BufferArray>       ExecutorBufferArrayTable;

        class ExecutorsInternal final
        {
        public:
            std::atomic<int64_t>                                                DefaultThreadId = 0;
            std::atomic<uint64_t>                                               TickCount = 0;
            std::shared_ptr<boost::asio::deadline_timer>                        Tick;
            DateTime                                                            Now;
            ExecutorContextPtr                                                  Default;
            ExecutorContextPtr                                                  Scheduler;
            SynchronizedObject                                                  Lock;
            ExecutorLinkedList                                                  ContextFifo;
            ExecutorTable                                                       ContextTable;
            ExecutorThreadTable                                                 Threads;
            ExecutorBufferArrayTable                                            Buffers;
            std::shared_ptr<Executors::Awaitable>                               NetstackExitAwaitable;

        public:
            ExecutorsInternal() noexcept;
        };

        static std::shared_ptr<ExecutorsInternal>                               Internal;
        Executors::ApplicationExitEventHandler                                  Executors::ApplicationExit;

        void Executors_cctor() noexcept
        {
            Internal = ppp::make_shared_object<ExecutorsInternal>();
        }

        static void Executors_Run(boost::asio::io_context& context) noexcept
        {
            auto run = [&context]() noexcept
                {
                    boost::asio::io_context::work work(context);
                    boost::system::error_code ec;
                    context.restart();
                    context.run(ec);
                };
#if defined(_WIN32)
            __try
            {
                run();
            }
            __except (ppp::win32::Win32Native::DumpApplicationAndExit(GetExceptionInformation())) {}
#else
            run();
#endif
        }

        static bool Executors_AwaitTickInternalLoops() noexcept
        {
            ExecutorContextPtr context = Internal->Default;
            if (NULL == context)
            {
                return false;
            }

            std::shared_ptr<boost::asio::deadline_timer> t = Internal->Tick;
            if (NULL == t)
            {
                return false;
            }

            boost::asio::deadline_timer::duration_type durationTime = ppp::threading::Timer::DurationTime(10);
            t->expires_from_now(durationTime);
            t->async_wait(
                [context, t](const boost::system::error_code& ec) noexcept {
                    if (ec) {
                        if (ec != boost::system::errc::operation_canceled) {
                            return;
                        }
                    }

                    Internal->Now = DateTime::Now();
                    Internal->TickCount
                        = ppp::GetTickCount();
                    Executors_AwaitTickInternalLoops();
                });
            return true;
        }

        static void Executors_DeleteTickByDefaultContext() noexcept
        {
            std::shared_ptr<boost::asio::deadline_timer> t = std::move(Internal->Tick);
            if (NULL != t)
            {
                Internal->Tick.reset();
                try {
                    boost::system::error_code ec;
                    t->cancel(ec);
                }
                catch (const std::exception&) {}
            }
        }

        static bool Executors_AddTickByDefaultContext() noexcept
        {
            ExecutorContextPtr context = Internal->Default;
            if (NULL == context)
            {
                return false;
            }
            else
            {
                Executors_DeleteTickByDefaultContext();
            }

            std::shared_ptr<boost::asio::deadline_timer> t = make_shared_object<boost::asio::deadline_timer>(*context);
            if (NULL == t)
            {
                return false;
            }
            else
            {
                Internal->Now = DateTime::Now();
                Internal->TickCount
                    = ppp::GetTickCount();
                Internal->Tick = std::move(t);
            }

            return Executors_AwaitTickInternalLoops();
        }

        static void Executors_DeleteCachedBuffer(const boost::asio::io_context* context) noexcept
        {
            ExecutorBufferArrayTable& buffers = Internal->Buffers;
            ExecutorBufferArrayTable::iterator tail = buffers.find(constantof(context));
            ExecutorBufferArrayTable::iterator endl = buffers.end();
            if (tail != endl)
            {
                buffers.erase(tail);
            }
        }

        static std::shared_ptr<boost::asio::io_context> Executors_AttachDefaultContext(const std::shared_ptr<BufferswapAllocator>& allocator) noexcept
        {
            SynchronizedObjectScope scope(Internal->Lock);
            if (NULL != Internal->Default)
            {
                return NULL;
            }

            std::shared_ptr<boost::asio::io_context> context = make_shared_object<boost::asio::io_context>();
            if (NULL == context)
            {
                return NULL;
            }

            Internal->Default = context;
            Internal->DefaultThreadId = GetCurrentThreadId();
            Internal->Buffers[context.get()] = BufferswapAllocator::MakeByteArray(allocator, PPP_BUFFER_SIZE);

            Executors_AddTickByDefaultContext();
            return context;
        }

        static std::shared_ptr<boost::asio::io_context> Executors_AddNewThreadContext(const std::shared_ptr<BufferswapAllocator>& allocator, int64_t threadId) noexcept
        {
            std::shared_ptr<boost::asio::io_context> context = make_shared_object<boost::asio::io_context>();
            if (NULL == context)
            {
                return NULL;
            }

            boost::asio::io_context* key = context.get();
            SynchronizedObjectScope scope(Internal->Lock);

            Internal->ContextFifo.emplace_back(context);
            Internal->ContextTable[threadId] = context;
            Internal->Threads[key] = Thread::GetCurrentThread();
            Internal->Buffers[key] = BufferswapAllocator::MakeByteArray(allocator, PPP_BUFFER_SIZE);
            return context;
        }

        static void Executors_EndNewThreadContext(int64_t threadId, const std::shared_ptr<boost::asio::io_context>& context) noexcept
        {
            ExecutorLinkedList& fifo = Internal->ContextFifo;
            ExecutorTable& contexts = Internal->ContextTable;
            ExecutorThreadTable& threads = Internal->Threads;
            SynchronizedObjectScope scope(Internal->Lock);

            auto CONTEXT_TABLE_TAIL = contexts.find(threadId);
            auto CONTEXT_TABLE_ENDL = contexts.end();
            if (CONTEXT_TABLE_TAIL != CONTEXT_TABLE_ENDL)
            {
                contexts.erase(CONTEXT_TABLE_TAIL);
            }

            auto CONTEXT_FIFO_ENDL = fifo.end();
            auto CONTEXT_FIFO_TAIL = std::find(fifo.begin(), CONTEXT_FIFO_ENDL, context);
            if (CONTEXT_FIFO_TAIL != CONTEXT_FIFO_ENDL)
            {
                fifo.erase(CONTEXT_FIFO_TAIL);
            }

            auto CONTEXT_THREAD_TAIL = threads.find(context.get());
            auto CONTEXT_THREAD_ENDL = threads.end();
            if (CONTEXT_THREAD_TAIL != CONTEXT_THREAD_ENDL)
            {
                threads.erase(CONTEXT_THREAD_TAIL);
            }

            Executors_DeleteCachedBuffer(context.get());
        }

        static void Executors_UnattachDefaultContext(const std::shared_ptr<boost::asio::io_context>& context) noexcept
        {
            SynchronizedObjectScope scope(Internal->Lock);
            Internal->DefaultThreadId = 0;
            Internal->Default.reset();

            Executors_DeleteTickByDefaultContext();
            Executors_DeleteCachedBuffer(context.get());
        }

        bool Executors_NetstackTryExit() noexcept
        {
            using Awaitable               = Executors::Awaitable;
            using SynchronizedObject      = std::mutex;
            using SynchronizedObjectScope = std::lock_guard<SynchronizedObject>;

            bool processed = false;
            std::shared_ptr<Awaitable> awaitable;
            for (;;)
            {
                static SynchronizedObject syncobj;
                SynchronizedObjectScope scope(syncobj);

                awaitable = Internal->NetstackExitAwaitable;
                lwip::netstack::close(
                    [awaitable]() noexcept  
                    {
                        if (NULL != awaitable) 
                        {
                            awaitable->Processed();
                        }
                    });

                if (NULL != awaitable)
                {
                    std::shared_ptr<boost::asio::io_context> executor = lwip::netstack::Executor;
                    if (NULL != executor)
                    {
                        bool stopped = executor->stopped();
                        if (!stopped)
                        {
                            processed = awaitable->Await();
                        }
                    }
                }

                Internal->NetstackExitAwaitable.reset();
                break;
            }

            return processed;
        }

        void Executors_NetstackAllocExitAwaitable() noexcept
        {
            Internal->NetstackExitAwaitable = make_shared_object<Executors::Awaitable>();
        }

        void Executors::GetAllContexts(ppp::vector<ContextPtr>& contexts) noexcept
        {
            bool any = false;
            SynchronizedObjectScope scope(Internal->Lock);
            for (auto&& kv : Internal->ContextTable)
            {
                any = true;
                contexts.emplace_back(kv.second);
            }

            if (!any)
            {
                ExecutorContextPtr context = Internal->Default;
                if (NULL != context)
                {
                    contexts.emplace_back(context);
                }
            }
        }

        std::shared_ptr<Byte> Executors::GetCachedBuffer(const std::shared_ptr<boost::asio::io_context>& context) noexcept
        {
            if (NULL == context)
            {
                return NULL;
            }

            ExecutorBufferArrayTable& buffers = Internal->Buffers;
            SynchronizedObjectScope scope(Internal->Lock);

            ExecutorBufferArrayTable::iterator tail = buffers.find(context.get());
            ExecutorBufferArrayTable::iterator endl = buffers.end();
            return tail != endl ? tail->second : NULL;
        }

        std::shared_ptr<boost::asio::io_context> Executors::GetCurrent() noexcept
        {
            int64_t threadId = GetCurrentThreadId();
            if (threadId != Internal->DefaultThreadId)
            {
                ExecutorTable& contexts = Internal->ContextTable;
                SynchronizedObjectScope scope(Internal->Lock);

                ExecutorTable::iterator tail = contexts.find(threadId);
                ExecutorTable::iterator endl = contexts.end();
                if (tail != endl)
                {
                    return tail->second;
                }
            }
            return Internal->Default;
        }

        std::shared_ptr<boost::asio::io_context> Executors::GetExecutor() noexcept
        {
            std::shared_ptr<boost::asio::io_context> context;
            do
            {
                ExecutorLinkedList& fifo = Internal->ContextFifo;
                ExecutorTable& contexts = Internal->ContextTable;
                SynchronizedObjectScope scope(Internal->Lock);
                if (contexts.size() == 1)
                {
                    ExecutorTable::iterator tail = contexts.begin();
                    context = tail->second;
                }
                else
                {
                    ExecutorLinkedList::iterator tail = fifo.begin();
                    ExecutorLinkedList::iterator endl = fifo.end();
                    if (tail != endl)
                    {
                        context = std::move(*tail);
                        fifo.erase(tail);
                        fifo.emplace_back(context);
                    }
                }
            } while (false);
            return Internal->Default;
        }

        std::shared_ptr<boost::asio::io_context> Executors::GetScheduler() noexcept
        {
            return Internal->Scheduler;
        }

        std::shared_ptr<boost::asio::io_context> Executors::GetDefault() noexcept
        {
            return Internal->Default;
        }

        int Executors::Run(const std::shared_ptr<BufferswapAllocator>& allocator, const ExecutorStart& start)
        {
            const char* argv[1] = {};
            int argc = 0;

            return Run(allocator, start, argc, argv);
        }

        int Executors::Run(const std::shared_ptr<BufferswapAllocator>& allocator, const ExecutorStart& start, int argc, const char* argv[])
        {
            if (NULL == start)
            {
                throw std::invalid_argument(nameof(start));
            }

            if (argc < 0)
            {
                throw std::invalid_argument(nameof(argc));
            }

            int return_code = -1;
            if (argc > 0 && NULL == argv)
            {
                throw std::invalid_argument(nameof(argv));
            }

            std::shared_ptr<boost::asio::io_context> context = Executors_AttachDefaultContext(allocator);
            if (NULL == context)
            {
                throw std::runtime_error("This operation cannot be repeated.");
            }
            else
            {
#if defined(_WIN32)
                ppp::win32::SYSTEM_WINDOWS_COM_INITIALIZED __SYSTEM_WINDOWS_COM_INITIALIZED__;
#endif
                context->post([&return_code, &start, argc, argv]() noexcept
                    {
                        return_code = start(argc, argv);
                        if (return_code != 0)
                        {
                            Executors::Exit();
                        }
                    });
                Executors_Run(*context);
            }

            Executors_UnattachDefaultContext(context);
            OnApplicationExit(context, return_code);
            return return_code;
        }

        void Executors::OnApplicationExit(const ContextPtr& context, int return_code) noexcept
        {
            // I'm letting go, I am finally willing to let go of your hands, because love you love to my heart.
            ApplicationExitEventHandler h = std::move(Executors::ApplicationExit);
            if (NULL != h)
            {
                h(return_code);
                Executors::ApplicationExit.reset();
            }
        }

        /* https://en.cppreference.com/w/cpp/thread/condition_variable */
        Executors::Awaitable::Awaitable() noexcept 
            : completed(false)
            , processed(false)
        {
        
        }

        void Executors::Awaitable::Processed() noexcept
        {
            LK lk(mtx);
            completed = true;
            processed = true;

            cv.notify_one();
        }

        bool Executors::Awaitable::Await() noexcept
        {
            LK lk(mtx);
            cv.wait(lk, [this]() noexcept {  return completed; });

            bool ok = false;
            ok = processed;
            processed = false;
            completed = false;

            return ok;
        }

        static bool Executors_CreateNewThread(const std::shared_ptr<BufferswapAllocator>& allocator) noexcept
        {
            std::shared_ptr<Executors::Awaitable> awaitable = make_shared_object<Executors::Awaitable>();
            if (NULL == awaitable)
            {
                return false;
            }

            std::weak_ptr<Executors::Awaitable> awaitable_weak = awaitable;
            std::shared_ptr<Thread> t = make_shared_object<Thread>(
                [allocator, awaitable_weak](Thread* my) noexcept
                {
                    int64_t threadId = GetCurrentThreadId();
                    if (std::shared_ptr<Executors::Awaitable> awaitable = awaitable_weak.lock(); NULL != awaitable)
                    {
                        awaitable->Processed();
                    }

                    std::shared_ptr<boost::asio::io_context> context = Executors_AddNewThreadContext(allocator, threadId);
                    if (NULL != context)
                    {
                        Executors_Run(*context);
                    }

                    Executors_EndNewThreadContext(threadId, context);
                });
            if (NULL == t)
            {
                return false;
            }

            t->SetPriority(ThreadPriority::Highest);
            if (!t->Start())
            {
                return false;
            }

            return awaitable->Await();
        }

        void Executors::SetMaxThreads(const std::shared_ptr<BufferswapAllocator>& allocator, int completionPortThreads) noexcept
        {
            if (completionPortThreads < 1)
            {
                completionPortThreads = 1;
            }

            ppp::vector<ExecutorContextPtr> releases;
            if (completionPortThreads)
            {
                ExecutorLinkedList& fifo = Internal->ContextFifo;
                ExecutorThreadTable& threads = Internal->Threads;
                ExecutorTable& contexts = Internal->ContextTable;
                SynchronizedObjectScope scope(Internal->Lock);

                for (int i = contexts.size(); i < completionPortThreads; i++)
                {
                    bool bok = Executors_CreateNewThread(allocator);
                    if (!bok)
                    {
                        break;
                    }
                }

                for (int i = completionPortThreads, max = contexts.size(); i < max; i++)
                {
                    auto CONTEXT_FIFO_TAIL = fifo.begin();
                    auto CONTEXT_FIFO_ENDL = fifo.end();
                    if (CONTEXT_FIFO_TAIL == CONTEXT_FIFO_ENDL)
                    {
                        break;
                    }

                    ExecutorContextPtr context = std::move(*CONTEXT_FIFO_TAIL);
                    fifo.erase(CONTEXT_FIFO_TAIL);

                    auto CONTEXT_THREAD_TAIL = threads.find(context.get()); 
                    auto CONTEXT_THREAD_ENDL = threads.end();
                    if (CONTEXT_THREAD_TAIL != CONTEXT_THREAD_ENDL)
                    {
                        auto& thread = CONTEXT_THREAD_TAIL->second; 
                        if (NULL != thread)
                        {
                            auto CONTEXT_TABLE_TAIL = contexts.find(thread->Id); 
                            auto CONTEXT_TABLE_ENDL = contexts.end();
                            if (CONTEXT_TABLE_TAIL != CONTEXT_TABLE_ENDL)
                            {
                                contexts.erase(CONTEXT_TABLE_TAIL);
                            }
                        }

                        threads.erase(CONTEXT_THREAD_TAIL);
                    }

                    releases.emplace_back(context);
                }
            }

            for (auto&& context : releases)
            {
                Exit(context);
            }
        }

        bool Executors::Exit(const std::shared_ptr<boost::asio::io_context>& context) noexcept
        {
            if (NULL == context)
            {
                return false;
            }

            bool stopped = context->stopped();
            if (stopped)
            {
                return false;
            }

            context->post(std::bind(&boost::asio::io_context::stop, context));
            return true;
        }

        bool Executors::Exit() noexcept
        {
            std::shared_ptr<ExecutorsInternal> i = Internal;
            if (NULL == i)
            {
                return false;
            }

            ExecutorContextPtr Default;
            ExecutorContextPtr Scheduler;
            ExecutorLinkedList ContextFifo;
            ExecutorTable ContextTable;
            ExecutorThreadTable Threads;
            {
                SynchronizedObjectScope scope(i->Lock);
                ContextFifo = i->ContextFifo;
                ContextTable = i->ContextTable;
                Threads = i->Threads;
                Default = i->Default;
                Scheduler = i->Scheduler;
            }

            bool any = false;
            for (auto&& context : ContextFifo)
            {
                any |= Exit(context);
            }

            for (auto&& [_, context] : ContextTable)
            {
                any |= Exit(context);
            }

            for (auto&& [_, thread] : Threads)
            {
                if (NULL != thread)
                {
                    thread->Join();
                }
            }

            Executors_NetstackTryExit();
            if (Exit(Scheduler))
            {
                any |= true;
            }

            if (Exit(Default))
            {
                any |= true;
            }

            return any;
        }

        DateTime Executors::Now() noexcept
        {
            std::shared_ptr<ExecutorsInternal> i = Internal;
            return NULL != i ? i->Now : DateTime::Now();
        }

        uint64_t Executors::GetTickCount() noexcept
        {
            std::shared_ptr<ExecutorsInternal> i = Internal;
            if (NULL != i)
            {
                std::shared_ptr<boost::asio::io_context> context = i->Default;
                if (NULL != context)
                {
                    return i->TickCount;
                }
            }

            return ppp::GetTickCount();
        }

        bool Executors::SetMaxSchedulers(int completionPortThreads) noexcept
        {
            if (completionPortThreads < 1)
            {
                completionPortThreads = 1;
            }

            SynchronizedObjectScope scope(Internal->Lock);
            if (NULL != Internal->Scheduler)
            {
                return true;;
            }

            ExecutorContextPtr scheduler = make_shared_object<boost::asio::io_context>();
            if (NULL == scheduler)
            {
                return false;
            }

#if defined(_WIN32)
            if (!ppp::win32::Win32Native::IsWindows81OrLaterVersion())
            {
                return false;
            }
#endif

            Internal->Scheduler = scheduler;
            for (int i = 0; i < completionPortThreads; i++)
            {
                std::shared_ptr<Thread> t = make_shared_object<Thread>(
                    [](Thread* my) noexcept
                    {
                        ExecutorContextPtr scheduler = Internal->Scheduler;
                        if (NULL != scheduler)
                        {
                            SetThreadPriorityToMaxLevel();
                            SetThreadName("scheduler");
                            Executors_Run(*scheduler);
                        }
                    });
                t->SetPriority(ThreadPriority::Highest);
                t->Start();
            }
            return true;
        }

        ExecutorsInternal::ExecutorsInternal() noexcept
        {
            lwip::netstack::close_event =
                [this]() noexcept
                {
                    std::shared_ptr<Executors::Awaitable> awaitable = std::move(NetstackExitAwaitable);
                    NetstackExitAwaitable.reset();

                    if (NULL != awaitable)
                    {
                        awaitable->Processed();
                    }
                };

            SetThreadPriorityToMaxLevel();
            SetProcessPriorityToMaxLevel();
        }

        bool Executors::ShiftToScheduler(ppp::threading::Executors::ContextPtr& context, ppp::threading::Executors::StrandPtr& strand) noexcept
        {
            context = GetScheduler();
            if (NULL == context)
            {
                context = ppp::threading::Executors::GetExecutor();
                return NULL != context;
            }
            else
            {
                strand = make_shared_object<Strand>(boost::asio::make_strand(*context));
                if (NULL != strand)
                {
                    return true;
                }
                else
                {
                    context = ppp::threading::Executors::GetExecutor();
                    return NULL != context;
                }
            }
        }
    }
}