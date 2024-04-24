#pragma once

#include <ppp/stdafx.h>
#include <boost/coroutine/detail/coroutine_context.hpp>
#include <boost/context/detail/fcontext.hpp>
#include <ppp/threading/Executors.h>
#include <ppp/threading/BufferswapAllocator.h>

namespace ppp
{
    namespace coroutines
    {
        class YieldContext final
        {
        public:
            typedef ppp::function<void(YieldContext&)>                          SpawnHander;

        public:
            bool                                                                Resume() noexcept;
            bool                                                                Suspend() noexcept;
            YieldContext*                                                       GetPtr() const noexcept        { return constantof(this);}
            boost::asio::io_context&                                            GetContext() const noexcept    { return context_; }
            boost::asio::strand<boost::asio::io_context::executor_type>*        GetStrand() const noexcept     { return strand_; }

        public:
            bool                                                                Y() noexcept { return Suspend(); }
            bool                                                                R() noexcept { return ppp::threading::Executors::Post(&context_, strand_, std::bind(&YieldContext::Resume, this)); }

        public:
            operator                                                            bool() const noexcept          { return NULL != GetPtr(); }
            operator                                                            YieldContext*() const noexcept { return GetPtr(); }

        public:
            static bool                                                         Spawn(boost::asio::io_context& context, SpawnHander&& spawn) noexcept
            {
                return YieldContext::Spawn(context, std::move(spawn), PPP_COROUTINE_STACK_SIZE);
            }
            static bool                                                         Spawn(boost::asio::io_context& context, SpawnHander&& spawn, int stack_size) noexcept
            {
                ppp::threading::BufferswapAllocator* allocator = NULL;
                return YieldContext::Spawn(allocator, context, std::move(spawn), stack_size);
            }
            static bool                                                         Spawn(ppp::threading::BufferswapAllocator* allocator, boost::asio::io_context& context, SpawnHander&& spawn) noexcept
            {
                return YieldContext::Spawn(allocator, context, std::move(spawn), PPP_COROUTINE_STACK_SIZE);
            }
            static bool                                                         Spawn(ppp::threading::BufferswapAllocator* allocator, boost::asio::io_context& context, SpawnHander&& spawn, int stack_size) noexcept
            {
                boost::asio::strand<boost::asio::io_context::executor_type>* strand = NULL;
                return YieldContext::Spawn(allocator, context, strand, std::move(spawn), PPP_COROUTINE_STACK_SIZE);
            }
            static bool                                                         Spawn(ppp::threading::BufferswapAllocator* allocator, boost::asio::io_context& context, boost::asio::strand<boost::asio::io_context::executor_type>* strand, SpawnHander&& spawn)
            {
                return YieldContext::Spawn(allocator, context, strand, std::move(spawn), PPP_COROUTINE_STACK_SIZE);
            }
            static bool                                                         Spawn(ppp::threading::BufferswapAllocator* allocator, boost::asio::io_context& context, boost::asio::strand<boost::asio::io_context::executor_type>* strand, SpawnHander&& spawn, int stack_size) noexcept;

        private:
            void                                                                Invoke() noexcept;
            static void                                                         Handle(boost::context::detail::transfer_t t) noexcept;
            static void                                                         Switch(boost::context::detail::transfer_t t, YieldContext* y) noexcept;

        private:
            template <typename T, typename... A>
            static T*                                                           New(ppp::threading::BufferswapAllocator* allocator, A&&... args) noexcept
            {
                if (NULL == allocator)
                {
                    T* p = (T*)Malloc(sizeof(T));
                    if (NULL == p)
                    {
                        return NULL;
                    }

                    return new (p) T(allocator, std::forward<A&&>(args)...);
                }
                else
                {
                    T* p = (T*)allocator->Alloc(sizeof(T));
                    if (NULL == p)
                    {
                        allocator = NULL;
                        return New<T>(allocator, std::forward<A&&>(args)...);
                    }

                    return new (p) T(allocator, std::forward<A&&>(args)...);
                }
            }

            template <typename T>
            static bool                                                         Release(T* p) noexcept
            {
                if (NULL == p)
                {
                    return false;
                }

                ppp::threading::BufferswapAllocator* const allocator = p->allocator_;
                p->~T();

                if (NULL == allocator)
                {
                    Mfree(p);
                }
                else
                {
                    allocator->Free(p);
                }

                return true;
            }

        private:
            YieldContext() = delete;
            YieldContext(YieldContext&&) = delete;
            YieldContext(const YieldContext&) = delete;
            YieldContext(ppp::threading::BufferswapAllocator* allocator, boost::asio::io_context& context, boost::asio::strand<boost::asio::io_context::executor_type>* strand, SpawnHander&& spawn, int stack_size) noexcept;
            ~YieldContext() noexcept;

        private:
            std::atomic<int>                                                    s_;
            boost::context::detail::fcontext_t                                  callee_;
            boost::context::detail::fcontext_t                                  caller_;
            SpawnHander                                                         h_;
            boost::asio::io_context&                                            context_;
            boost::asio::strand<boost::asio::io_context::executor_type>*        strand_;
            int                                                                 stack_size_;
            std::shared_ptr<Byte>                                               stack_;
            ppp::threading::BufferswapAllocator*                                allocator_;
        };
    }
}