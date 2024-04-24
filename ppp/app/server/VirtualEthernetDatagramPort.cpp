#include <ppp/app/server/VirtualEthernetDatagramPort.h>
#include <ppp/app/server/VirtualEthernetExchanger.h>
#include <ppp/app/server/VirtualEthernetSwitcher.h>
#include <ppp/app/protocol/VirtualEthernetPacket.h>
#include <ppp/net/Socket.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/coroutines/asio/asio.h>
#include <ppp/coroutines/YieldContext.h>

typedef ppp::coroutines::YieldContext                   YieldContext;
typedef ppp::net::IPEndPoint                            IPEndPoint;
typedef ppp::net::Socket                                Socket;
typedef ppp::net::Ipep                                  Ipep;
typedef ppp::app::protocol::VirtualEthernetPacket       VirtualEthernetPacket;

namespace ppp {
    namespace app {
        namespace server {
            VirtualEthernetDatagramPort::VirtualEthernetDatagramPort(const VirtualEthernetExchangerPtr& exchanger, const ITransmissionPtr& transmission, const boost::asio::ip::udp::endpoint& sourceEP) noexcept
                : disposed_(false)
                , onlydns_(true)
                , sendto_(false)
                , in_(false)
                , finalize_(false)
                , timeout_(0)
                , context_(transmission->GetContext())
                , socket_(*context_)
                , exchanger_(exchanger)
                , transmission_(transmission)
                , configuration_(exchanger->GetConfiguration())
                , sourceEP_(sourceEP) {
                buffer_ = Executors::GetCachedBuffer(context_);
                Update();
            }

            VirtualEthernetDatagramPort::~VirtualEthernetDatagramPort() noexcept {
                Finalize();
            }

            std::shared_ptr<VirtualEthernetDatagramPort> VirtualEthernetDatagramPort::GetReference() noexcept {
                return shared_from_this();
            }

            VirtualEthernetDatagramPort::VirtualEthernetExchangerPtr VirtualEthernetDatagramPort::GetExchanger() noexcept {
                return exchanger_;
            }

            VirtualEthernetDatagramPort::ContextPtr VirtualEthernetDatagramPort::GetContext() noexcept {
                return context_;
            }

            VirtualEthernetDatagramPort::AppConfigurationPtr VirtualEthernetDatagramPort::GetConfiguration() noexcept {
                return configuration_;
            }

            boost::asio::ip::udp::endpoint& VirtualEthernetDatagramPort::GetLocalEndPoint() noexcept {
                return localEP_;
            }

            boost::asio::ip::udp::endpoint& VirtualEthernetDatagramPort::GetSourceEndPoint() noexcept {
                return sourceEP_;
            }

            void VirtualEthernetDatagramPort::Finalize() noexcept {
                std::shared_ptr<ITransmission> transmission = std::move(transmission_); 
                transmission_.reset();
                
                if (sendto_ && !finalize_) {
                    if (transmission) {
                        if (!exchanger_->DoSendTo(transmission, sourceEP_, sourceEP_, NULL, 0, nullof<YieldContext>())) {
                            transmission->Dispose();
                        }
                    }
                }

                disposed_ = true;
                sendto_ = false;
                finalize_ = true;
                Socket::Closesocket(socket_);

                exchanger_->ReleaseDatagramPort(sourceEP_);
            }

            void VirtualEthernetDatagramPort::Dispose() noexcept {
                auto self = shared_from_this();
                std::shared_ptr<boost::asio::io_context> context = GetContext();
                context->post(
                    [self, this]() noexcept {
                        Finalize();
                    });
            }

            bool VirtualEthernetDatagramPort::Open() noexcept {
                if (disposed_) {
                    return false;
                }

                bool opened = socket_.is_open();
                if (opened) {
                    return false;
                }

                std::shared_ptr<VirtualEthernetSwitcher> switcher = exchanger_->GetSwitcher();
                boost::asio::ip::address address = switcher->GetInterfaceIP();

                bool success = VirtualEthernetPacket::OpenDatagramSocket(socket_, address, IPEndPoint::MinPort, sourceEP_) && Loopback();
                if (success) {
                    boost::system::error_code ec;
                    localEP_ = socket_.local_endpoint(ec);
                    if (ec) {
                        return false;
                    }

                    boost::asio::ip::address localIP = localEP_.address();
                    in_ = localIP.is_v4();

                    int handle = socket_.native_handle();
                    ppp::net::Socket::AdjustDefaultSocketOptional(handle, in_);
                    ppp::net::Socket::SetTypeOfService(handle);
                    ppp::net::Socket::SetSignalPipeline(handle, false);
                    ppp::net::Socket::ReuseSocketAddress(handle, true);
                }

                return success;
            }

            bool VirtualEthernetDatagramPort::Loopback() noexcept {
                if (disposed_) {
                    return false;
                }

                bool opened = socket_.is_open();
                if (!opened) {
                    return false;
                }

                auto self = shared_from_this();
                socket_.async_receive_from(boost::asio::buffer(buffer_.get(), PPP_BUFFER_SIZE), remoteEP_,
                    [self, this](const boost::system::error_code& ec, std::size_t sz) noexcept {
                        bool disposing = true;
                        while (ec == boost::system::errc::success) {
                            int bytes_transferred = sz;
                            if (bytes_transferred < 1) {
                                disposing = false;
                                break;
                            }

                            std::shared_ptr<ITransmission> transmission = transmission_;
                            if (!transmission) {
                                break;
                            }

                            boost::asio::ip::udp::endpoint remoteEP = Ipep::V6ToV4(remoteEP_);
                            if (exchanger_->DoSendTo(transmission, sourceEP_, remoteEP, buffer_.get(), bytes_transferred, nullof<YieldContext>())) {
                                disposing = false;
                            }
                            else {
                                transmission_.reset();
                                transmission->Dispose();
                            }

                            break;
                        }

                        if (disposing) {
                            Dispose();
                        }
                        else {
                            Loopback();
                        }
                    });
                return true;
            }

            bool VirtualEthernetDatagramPort::SendTo(const void* packet, int packet_length, const boost::asio::ip::udp::endpoint& destinationEP) noexcept {
                if (NULL == packet || packet_length < 1) {
                    return false;
                }

                if (disposed_) {
                    return false;
                }

                bool opened = socket_.is_open();
                if (!opened) {
                    return false;
                }

                int destinationPort = destinationEP.port();
                if (destinationPort <= IPEndPoint::MinPort || destinationPort > IPEndPoint::MaxPort) {
                    return false;
                }

                boost::system::error_code ec;
                if (in_) {
                    socket_.send_to(boost::asio::buffer(packet, packet_length), 
                        Ipep::V6ToV4(destinationEP), boost::asio::socket_base::message_end_of_record, ec);
                }
                else {
                    socket_.send_to(boost::asio::buffer(packet, packet_length), 
                        Ipep::V4ToV6(destinationEP), boost::asio::socket_base::message_end_of_record, ec);
                }

                if (ec) {
                    return false; // Failed to sendto the datagram packet. 
                }
                else {
                    // Succeeded in sending the datagram packet to the external network. 
                    sendto_ = true;
                    if (destinationPort != PPP_DNS_SYS_PORT) {
                        onlydns_ = false;
                    }

                    Update();
                    return true;
                }
            }
        }
    }
}