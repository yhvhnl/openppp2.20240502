#include <ppp/coroutines/YieldContext.h>

namespace ppp
{
    namespace coroutines
    {
        YieldContext::YieldContext(ppp::threading::BufferswapAllocator* allocator, boost::asio::io_context& context, boost::asio::strand<boost::asio::io_context::executor_type>* strand, SpawnHander&& spawn, int stack_size) noexcept
            : s_(FALSE)
            , callee_(NULL)
            , caller_(NULL)
            , h_(std::move(spawn))
            , context_(context)
            , strand_(strand)
            , stack_size_(stack_size)
            , allocator_(allocator)
        {
            if (allocator)
            {
                Byte* stack = (Byte*)allocator->Alloc(stack_size);
                if (stack)
                {
                    stack_ = std::shared_ptr<Byte>(stack,
                        std::bind(&ppp::threading::BufferswapAllocator::Free, allocator, std::placeholders::_1));
                }
            }

            /* boost::context::stack_traits::minimum_size(); */
            if (!stack_)
            {
                stack_ = make_shared_alloc<Byte>(stack_size);
            }
        }

        YieldContext::~YieldContext() noexcept
        {
            YieldContext* y = this;
            y->h_          = NULL;
            y->stack_      = NULL;
            y->stack_size_ = 0;
            y->strand_     = NULL;
            y->allocator_  = NULL;
        }

        bool YieldContext::Suspend() noexcept
        {
            int L = FALSE;
            if (s_.compare_exchange_strong(L, TRUE))
            {
                YieldContext* y = this;
                y->caller_ = boost::context::detail::jump_fcontext(y->caller_, y).fctx;
                return true;
            }

            return false;
        }

        bool YieldContext::Resume() noexcept
        {
            int L = TRUE;
            if (s_.compare_exchange_strong(L, FALSE))
            {
                YieldContext* y = this;
                Switch(boost::context::detail::jump_fcontext(y->callee_, y), y);
                return true;
            }

            return false;
        }

        void YieldContext::Invoke() noexcept
        {
            YieldContext* y = this;
            if (Byte* stack = stack_.get(); stack)
            {
                boost::context::detail::fcontext_t callee =
                    boost::context::detail::make_fcontext(stack + stack_size_, stack_size_, &YieldContext::Handle);
                Switch(boost::context::detail::jump_fcontext(callee, y), y);
            }
            else
            {
                YieldContext::Release(y);
            }
        }

        void YieldContext::Switch(boost::context::detail::transfer_t t, YieldContext* y) noexcept
        {
            if (t.data)
            {
                y->callee_ = t.fctx;
            }
            else
            {
                YieldContext::Release(y);
            }
        }

        void YieldContext::Handle(boost::context::detail::transfer_t t) noexcept
        {
            YieldContext* y = (YieldContext*)t.data;
            if (y)
            {
                SpawnHander h = std::move(y->h_);
                y->h_ = NULL;
                y->caller_ = t.fctx;
                
                if (h)
                {
                    h(*y);
                    h = NULL;
                }

                std::atomic<int>& s = y->s_;
                if (s.exchange(FALSE))
                {
                    Switch(boost::context::detail::jump_fcontext(y->callee_, y), y);
                }

                boost::context::detail::jump_fcontext(y->caller_, NULL);
            }
        }
 
        bool YieldContext::Spawn(ppp::threading::BufferswapAllocator* allocator, boost::asio::io_context& context, boost::asio::strand<boost::asio::io_context::executor_type>* strand, SpawnHander&& spawn, int stack_size) noexcept
        {
            if (!spawn)
            {
                return false;
            }

            int pagesize = GetMemoryPageSize();
            stack_size = std::max<int>(stack_size, pagesize);

            // If done on the thread that owns the context, it is executed immediately.
            // Otherwise, the delivery event is delivered to the actor queue of the context, 
            // And the host thread of the context drives it when the next event is triggered.
            YieldContext* y = New<YieldContext>(allocator, context, strand, std::move(spawn), stack_size);
            if (!y)
            {
                return false;
            }

            // By default the C/C++ compiler optimizes the context delegate event call, and strand is usually multi-core driven if it occurs.
            if (strand)
            {
                boost::asio::post(*strand, 
                    std::bind(&YieldContext::Invoke, y));
            }
            else
            {
                context.post(std::bind(&YieldContext::Invoke, y));
            }

            return true;
        }
    }
}