#pragma once

#include <ppp/stdafx.h>
#include <ppp/threading/Timer.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/net/asio/asio.h>
#include <ppp/coroutines/YieldContext.h>

namespace ppp {
    namespace coroutines {
        namespace asio {
            template <typename AsyncWriteStream, typename MutableBufferSequence>
            bool                                                                async_read(AsyncWriteStream& stream, const MutableBufferSequence& buffers, YieldContext& y) noexcept {
                if (!buffers.data() || !buffers.size()) {
                    return false;
                }

                int len = -1;
                boost::asio::post(stream.get_executor(),
                    [&stream, &buffers, &y, &len]() noexcept {
                        boost::asio::async_read(stream, constantof(buffers),
                            [&y, &len](const boost::system::error_code& ec, std::size_t sz) noexcept {
                                len = std::max<int>(ec ? -1 : sz, -1);
                                y.R();
                            });
                    });

                y.Suspend();
                return len == buffers.size();
            }

            template <typename AsyncWriteStream, typename ConstBufferSequence>
            bool                                                                async_write(AsyncWriteStream& stream, const ConstBufferSequence& buffers, YieldContext& y) noexcept {
                if (!buffers.data() || !buffers.size()) {
                    return false;
                }

                bool ok = false;
                boost::asio::post(stream.get_executor(),
                    [&stream, &buffers, &y, &ok]() noexcept {
                        boost::asio::async_write(stream, constantof(buffers),
                            [&y, &ok](const boost::system::error_code& ec, std::size_t sz) noexcept {
                                ok = ec == boost::system::errc::success; /* b is boost::system::errc::success. */
                                y.R();
                            });
                    });

                y.Suspend();
                return ok;
            }

            template <typename AsyncWriteStream, typename MutableBufferSequence>
            int                                                                 async_read_some(AsyncWriteStream& stream, const MutableBufferSequence& buffers, YieldContext& y) noexcept {
                int len = -1;
                if (!buffers.data() || !buffers.size()) {
                    return len;
                }

                boost::asio::post(stream.get_executor(),
                    [&stream, &buffers, &y, &len]() noexcept {
                        stream.async_read_some(constantof(buffers),
                            [&y, &len](const boost::system::error_code& ec, std::size_t sz) noexcept {
                                len = std::max<int>(ec ? -1 : sz, -1);
                                y.R();
                            });
                    });

                y.Suspend();
                return len;
            }

            inline bool                                                         async_sleep(YieldContext& y, int milliseconds) noexcept {
                return ppp::threading::Timer::Timeout(milliseconds, y);
            }

            inline bool                                                         async_connect(boost::asio::ip::tcp::socket& socket, const boost::asio::ip::tcp::endpoint& remoteEP, YieldContext& y) noexcept {
                boost::asio::ip::address address = remoteEP.address();
                if (ppp::net::IPEndPoint::IsInvalid(address)) {
                    return false;
                }

                int port = remoteEP.port();
                if (port <= ppp::net::IPEndPoint::MinPort || port > ppp::net::IPEndPoint::MaxPort) {
                    return false;
                }

                bool ok = false;
                boost::asio::post(socket.get_executor(), 
                    [&socket, &remoteEP, &y, &ok]() noexcept {
                        socket.async_connect(remoteEP,
                            [&y, &ok](const boost::system::error_code& ec) noexcept {
                                ok = ec == boost::system::errc::success; /* b is boost::system::errc::success. */
                                y.R();
                            });
                        });

                y.Suspend();
                return ok;
            }

            template <class AsyncSocket, class TProtocol>
            bool                                                                async_open(YieldContext& y, AsyncSocket& socket, const TProtocol& protocol) noexcept {
                // Android platform fatal system network underlying library bug, if in stackful coroutine, call socket, connect function will crash directly, 
                // In order to solve this problem, need to delegate to the android framework thread (Fwmark) to call, 
                // Will ensure that the program does not crash. It's just... Inexplicable.
                // 
                // Refer:
                //  https://android.googlesource.com/platform/frameworks/base.git/+/android-4.2.2_r1/core/jni/AndroidRuntime.cpp
                //  https://android.googlesource.com/platform/system/netd/+/master/client/FwmarkClient.cpp
#if defined(_ANDROID)
                bool ok = false;
                boost::asio::post(socket.get_executor(),
                    [&socket, &protocol, &ok, &y]() noexcept {
                        boost::system::error_code ec;
                        socket.open(protocol, ec);

                        if (ec == boost::system::errc::success) {
                            ok = true;
                        }

                        y.R();
                    });

                y.Suspend();
                return ok;
#else
                boost::system::error_code ec;
                socket.open(protocol, ec);

                return ec == boost::system::errc::success;
#endif
            }

            template <class TProtocol>
            boost::asio::ip::basic_endpoint<TProtocol>                          GetAddressByHostName(boost::asio::ip::basic_resolver<TProtocol>& resolver, const char* hostname, int port, YieldContext& y) noexcept {
                typedef boost::asio::ip::basic_resolver<TProtocol> protocol_resolver;

                auto fx = [&y](protocol_resolver& resolver, typename protocol_resolver::query& q, boost::system::error_code& ec) noexcept {
#if !defined(_WIN32)
                    using results_iterator = typename protocol_resolver::iterator;

                    results_iterator r{};
                    boost::asio::post(resolver.get_executor(),
                        [&resolver, &q, &y, &ec, &r]() noexcept {
                            resolver.async_resolve(q,
                                [&y, &ec, &r](const boost::system::error_code& e_, const results_iterator& i_) noexcept {
                                    ec = e_;
                                    r = i_;

                                    y.R();
                                });
                        });
#else
                    using results_type = typename protocol_resolver::results_type;

                    results_type r{};
                    boost::asio::post(resolver.get_executor(),
                        [&resolver, &q, &y, &ec, &r]() noexcept {
                            resolver.async_resolve(q,
                                [&y, &ec, &r](const boost::system::error_code& e_, const results_type& i_) noexcept {
                                    ec = e_;
                                    r = i_;

                                    y.R();
                                });
                        });
#endif
                    y.Suspend();
                    return r;
                };
                return ppp::net::asio::internal::GetAddressByHostName(resolver, hostname, port, fx);
            }
        }
    }
}