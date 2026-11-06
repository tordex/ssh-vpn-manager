#include "secret_store.hpp"

#include <libsecret/secret.h>

namespace
{

    const SecretSchema* password_schema()
    {
        static const SecretSchema schema = {
            "org.sshvpnmanager.SshVpnManager.Password",
            SECRET_SCHEMA_NONE,
            {
              {"id", SECRET_SCHEMA_ATTRIBUTE_STRING},
              {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}, // sentinel
            },
            /* private */
            0,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
        };
        return &schema;
    }

} // namespace

bool SecretStore::set_password(const std::string& id, const std::string& password, std::string* error)
{
    GError*           gerr  = nullptr;
    const std::string label = "SSH VPN Manager: " + id;
    const gboolean    ok    = secret_password_store_sync(password_schema(), SECRET_COLLECTION_DEFAULT, label.c_str(),
                                                         password.c_str(), nullptr, &gerr, "id", id.c_str(), nullptr);
    if(!ok)
    {
        if(error && gerr)
        {
            *error = gerr->message;
        }
        if(gerr)
        {
            g_error_free(gerr);
        }
        return false;
    }
    return true;
}

std::optional<std::string> SecretStore::get_password(const std::string& id, std::string* error)
{
    GError* gerr = nullptr;
    gchar*  pw   = secret_password_lookup_sync(password_schema(), nullptr, &gerr, "id", id.c_str(), nullptr);
    if(gerr)
    {
        if(error)
        {
            *error = gerr->message;
        }
        g_error_free(gerr);
        return std::nullopt;
    }
    if(!pw)
    {
        if(error)
        {
            *error = "no password stored for this server";
        }
        return std::nullopt;
    }
    std::string result(pw);
    secret_password_free(pw);
    return result;
}

bool SecretStore::delete_password(const std::string& id, std::string* error)
{
    GError*        gerr = nullptr;
    const gboolean ok   = secret_password_clear_sync(password_schema(), nullptr, &gerr, "id", id.c_str(), nullptr);
    if(!ok)
    {
        if(error && gerr)
        {
            *error = gerr->message;
        }
        if(gerr)
        {
            g_error_free(gerr);
        }
        return false;
    }
    return true;
}
