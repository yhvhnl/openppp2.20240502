#include <ppp/auxiliary/StringAuxiliary.h>
#include <ppp/net/Ipep.h>

namespace ppp 
{
    namespace auxiliary 
    {
        Int128 StringAuxiliary::GuidStringToInt128(const ppp::string& guid_string) noexcept 
        {
            if (guid_string.empty()) 
            {
                return 0;
            }

            boost::uuids::uuid guid = StringToGuid(guid_string);
            return ppp::net::Ipep::NetworkToHostOrder(*(Int128*)guid.data);
        }

        ppp::string StringAuxiliary::Int128ToGuidString(const Int128& guid) noexcept 
        {
            boost::uuids::uuid uuid;
            *(Int128*)uuid.data = ppp::net::Ipep::HostToNetworkOrder(guid);

            return GuidToString(uuid);
        }

        bool StringAuxiliary::WhoisIntegerValueString(const ppp::string& integer_string) noexcept
        {
            int integer_size = integer_string.size();
            if (integer_size < 1)
            {
                return false;
            }

            const char* integer_string_memory = integer_string.data();
            for (int i = 0; i < integer_size; i++)
            {
                char ch = integer_string_memory[i];
                if (ch >= '0' && ch <= '9')
                {
                    continue;
                }

                if (i == 0)
                {
                    if (ch == '-' || ch == '+')
                    {
                        continue;
                    }
                }
                return false;
            }
            return true;
        }

        ppp::string StringAuxiliary::Lstrings(const ppp::string& in, bool colon) noexcept
        {
            if (in.empty()) 
            {
                return ppp::string();
            }

            ppp::string result = in;
            if (colon)
            {
                result = Replace<ppp::string>(result, ":", ",");
            }

            result = Replace<ppp::string>(result, ";", ",");
            result = Replace<ppp::string>(result, " ", ",");
            result = Replace<ppp::string>(result, "|", ",");
            result = Replace<ppp::string>(result, "+", ",");
            result = Replace<ppp::string>(result, "*", ",");
            result = Replace<ppp::string>(result, "^", ",");
            result = Replace<ppp::string>(result, "&", ",");
            result = Replace<ppp::string>(result, "#", ",");
            result = Replace<ppp::string>(result, "@", ",");
            result = Replace<ppp::string>(result, "!", ",");
            result = Replace<ppp::string>(result, "'", ",");
            result = Replace<ppp::string>(result, "\"", ",");
            result = Replace<ppp::string>(result, "?", ",");
            result = Replace<ppp::string>(result, "%", ",");
            result = Replace<ppp::string>(result, "[", ",");
            result = Replace<ppp::string>(result, "]", ",");
            result = Replace<ppp::string>(result, "{", ",");
            result = Replace<ppp::string>(result, "}", ",");
            result = Replace<ppp::string>(result, "\\", ",");
            result = Replace<ppp::string>(result, "/", ",");
            result = Replace<ppp::string>(result, "-", ",");
            result = Replace<ppp::string>(result, "_", ",");
            result = Replace<ppp::string>(result, "=", ",");
            result = Replace<ppp::string>(result, "`", ",");
            result = Replace<ppp::string>(result, "~", ",");
            result = Replace<ppp::string>(result, "\r", ",");
            result = Replace<ppp::string>(result, "\n", ",");
            result = Replace<ppp::string>(result, "\t", ",");
            result = Replace<ppp::string>(result, "\a", ",");
            result = Replace<ppp::string>(result, "\b", ",");
            result = Replace<ppp::string>(result, "\v", ",");
            result = Replace<ppp::string>(result, "\f", ",");
            return result;
        }
    }
}