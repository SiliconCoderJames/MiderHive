#pragma once
// 更新清单签名：公钥常量与验签包装。
//
// 信任模型：latest.json 的原始字节由维护者的 RSA-2048 私钥签名（PKCS1v15/SHA256），
// 签名以 base64 放在旁边的 latest.json.sig 资产里。私钥只存在于两处——CI 的
// GitHub Secrets（MIDERHIVE_UPDATE_SIGNING_KEY，发版时签发）与维护者本机
// ~/.miderhive-keys/update-signing.pem——绝不入库。公钥编译进应用：渠道上的
// latest.json 即便被篡改，签名对不上就会被更新器拒绝（哈希防损坏，签名防渠道
// 篡改；安装包本体仍未签名，SmartScreen 提示依旧）。
//
// 换钥：新钥先签发 + 过渡版本同时内置新旧两把公钥 → 一个大版本后移除旧钥。
// 过渡期校验逻辑（任一公钥验签通过即信任）需要在此文件扩展成常量数组。
#include <string>

#include "core/util.h"

namespace ah {

// RSA-2048 公钥（openssl rsa -modulus 输出口径的大端 hex；指数 65537 = "010001"）
inline constexpr const char* kUpdateKeyModulusHex =
    "956F3C7B30AB460C0E54AC43E409CC1086CB8200C9FE7E75FE8FF408E38A5B8C3CAE18B4AB6CE200788E75341CAC1D4DF74500A57037EFA9"
    "443178CE5E22833AF36F042C584DFCAF501A6DB2AE294AF4A93A9BF03417771ADC8DB55C37432C7EA3181A9FF3CE815F55EBB38AEC55FEA18"
    "FE828F071ED7E6CD424CC7C8D2B1F916EE5EAB5A18DD37A4C0E6457D0DF1BE27EDF9615A8B97BB3F0893F21ECD35E221FD6BCC351259A0F4"
    "6F8720562F819BE5B59CEE99414EC087AD7A323F2E5818A700EE3FF1B965B2BD141C9CC97B9F86411BBBFF3830B06DACD0D174ED0B91FED32"
    "1166E6FE775F4CED6C9F97C6BE1B8F3EE59054AF501D7A04E404BD0684AB2D";
inline constexpr const char* kUpdateKeyExponentHex = "010001";

// 验证更新清单签名：manifest 为 latest.json 的原始字节，sig 为 .sig 文件内容（base64）
inline bool verifyUpdateManifest(const std::string& manifest, const std::string& sigBase64) {
    return rsaVerifySha256Pkcs1(kUpdateKeyModulusHex, kUpdateKeyExponentHex, manifest, sigBase64);
}

}  // namespace ah
