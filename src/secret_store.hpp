#pragma once

#include <optional>
#include <string>

// Encrypts passwords at rest via the freedesktop Secret Service (GNOME Keyring /
// KWallet / keepassxc daemon). Each password is stored under the server's `id`
// attribute. Plaintext is only ever held transiently while a tunnel connects.
class SecretStore {
public:
  bool set_password(const std::string& id, const std::string& password,
                    std::string* error);
  std::optional<std::string> get_password(const std::string& id, std::string* error);
  bool delete_password(const std::string& id, std::string* error);
};
