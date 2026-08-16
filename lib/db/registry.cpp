#ifdef HTTPLIB_ENABLED_DATABASE
#include "registry.hpp"
#include <map>
#include <mutex>
#include <utility>

namespace httplib::db::detail
{
    namespace
    {
        using registry_type = std::map<std::string, backend_factory, std::less<>>;

        registry_type&
        backend_registry()
        {
            static registry_type m;
            return m;
        }

        std::mutex&
        registry_mutex()
        {
            static std::mutex m;
            return m;
        }
    } // namespace

    HTTPLIB_API bool
    register_backend(std::string_view name, backend_factory factory)
    {
        std::lock_guard<std::mutex> lk(registry_mutex());
        return backend_registry().emplace(std::string(name), std::move(factory)).second;
    }

    backend_factory const*
    find_backend(std::string_view name)
    {
        std::lock_guard<std::mutex> lk(registry_mutex());
        auto it = backend_registry().find(name);
        return it == backend_registry().end() ? nullptr : &it->second;
    }

    std::string
    registered_backend_names()
    {
        std::lock_guard<std::mutex> lk(registry_mutex());
        std::string names;
        for (auto const& [name, _] : backend_registry())
        {
            if (!names.empty())
            {
                names += ", ";
            }
            names += name;
        }
        return names;
    }

    void
    register_backends()
    {
        static std::once_flag flag;
        std::call_once(flag,
                       []
                       {
                           register_mysql_backend();
                           register_sqlite_backend();
                           register_odbc_backend();
                       });
    }

} // namespace httplib::db::detail
#endif // HTTPLIB_ENABLED_DATABASE
