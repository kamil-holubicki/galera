/* Copyright (c) 2022 Percona LLC and/or its affiliates. All rights
   reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef __GU_ENCRYPTION__
#define __GU_ENCRYPTION__

#include <string>
#include <functional>

namespace gu {
class UUID;
class Config;

#define ptr2ull(ptr) ((unsigned long long)ptr)

std::string encode64(const std::string& binary);
std::string decode64(const std::string& base64);
std::string generateRandomKey();
std::string EncryptKey(const std::string &keyToBeEncrypted, const std::string &key);
std::string DecryptKey(const std::string &keyToBeDecrypted, const std::string &key);
std::string CreateMasterKeyName(UUID& uuid, int keyId);

class MasterKeyProvider {
public:
    MasterKeyProvider(std::function<std::string(const std::string&)> getKeyCb,
      std::function<bool(const std::string&)> createKeyCb);
    void RegisterKeyRotationRequestObserver(std::function<bool()> fn);
    bool NotifyKeyRotationObserver();
    std::string GetKey(const std::string& keyId);
    bool CreateKey(const std::string& keyId);

private:
    std::function<bool()> keyRotationObserver_;
    std::function<std::string(const std::string&)> getKeyCb_;
    std::function<bool(const std::string&)> createKeyCb_;
};

}
#endif  /* __GU_ENCRYPTION__ */