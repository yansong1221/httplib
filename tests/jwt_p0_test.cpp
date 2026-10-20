// =============================================================================
// P0 设计审查验证用例 — JWT 部分（DESIGN_REVIEW.md §1.1 / §1.2）—— 已修复。
//
// 历史用途：写"断言期望行为"的用例，在旧实现（手写 HMAC/base64/校验）下失败以
// 证实缺陷；现为**回归测试**（[p0][jwt][regression]），修复后必须保持通过。
// 修复方式：jwt 模块整体委托 jwt-cpp（HMAC/base64）+ verifier 改 fail-closed
// 白名单（未配算法 → error::not_initialized）+ sign() 只更新 alg/typ 不再重置
// header claims。见 lib/jwt.cpp 顶部注释。
//
// 覆盖：
//   P0-1 verifier 未调用 allow_algorithm() 时完全不校验签名（fail-open）
//        —— lib/jwt.cpp:567-579：签名校验整体包在 `if (alg_)` 里，alg_ 为空时
//           只校验明文 claims，任何伪造 token 直接放行。
//   P0-2 builder::sign() 重置 header_（lib/jwt.cpp:464-476），sign() 之前
//        set_key_id / set_content_type / set_header_claim 设置的值被静默丢弃。
// =============================================================================

#include "common.hpp"
#include "httplib/jwt.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>

// ===========================================================================
// P0-1：未配置算法的 verifier 必须 fail-closed
// ===========================================================================

TEST_CASE("P0-1: verifier without allow_algorithm() must reject a forged token (fail-closed)",
          "[p0][jwt][regression]")
{
    // control：配置了算法（但密钥不匹配）的 verifier 会拒绝伪造 token。
    //          证明：（a）伪造 token 确实是"假签名"；（b）校验断言本身可靠。
    auto good_secret = httplib::jwt::hs256("real-secret");

    auto victim_token = httplib::jwt::create()
                            .set_issuer("my-app")
                            .set_subject("victim")
                            .set_expires_in(std::chrono::hours { 1 })
                            .sign(good_secret);
    auto victim_decoded = httplib::jwt::decode(victim_token).value();

    // 攻击者用**自己的密钥**签同样的 claims —— 签名校验理应失败。
    auto attacker_token = httplib::jwt::create()
                              .set_issuer("my-app")
                              .set_subject("victim")
                              .set_expires_in(std::chrono::hours { 1 })
                              .sign(httplib::jwt::hs256("attacker-key"));
    auto attacker_decoded = httplib::jwt::decode(attacker_token).value();

    INFO("control：配置 secret 的 verifier 拒绝攻击者签名 —— 断言/脚手架可靠");
    boost::system::error_code control_ec;
    httplib::jwt::verify(good_secret).verify(attacker_decoded, control_ec);
    REQUIRE(control_ec == make_error_code(httplib::jwt::error::signature_verification));

    // ---- 验证点：verifier 不调用 allow_algorithm() 必须 fail-closed ----
    // 已修复：verify() 立即返回 error::not_initialized（旧实现会跌进 claims 校验，
    // iss/exp 全部匹配 ⇒ 返回空 ec，伪造 token 被静默放行）。
    httplib::jwt::verifier unconfigured;
    boost::system::error_code ec;
    unconfigured.verify(attacker_decoded, ec);
    INFO("修复后：未配置算法的 verifier 必须报错，不得放行伪造 token");
    REQUIRE(ec == make_error_code(httplib::jwt::error::not_initialized));
}

// ===========================================================================
// P0-2：builder::sign() 不应吞掉此前设置的 header claims（kid/cty/自定义）
// ===========================================================================

TEST_CASE("P0-2: set_key_id/set_content_type/set_header_claim must survive sign()",
          "[p0][jwt][regression]")
{
    // control：set_type 走独立的 type_ 成员，sign() 里单独回填 —— 当前即正常。
    auto control_token = httplib::jwt::create()
                             .set_type("JWT")
                             .set_key_id("k1")
                             .set_content_type("at+jwt")
                             .set_header_claim("x-header-tag", boost::json::value("v1"))
                             .sign(httplib::jwt::hs256("secret"));
    auto control_hdr = httplib::jwt::decode(control_token).value().get_header();
    REQUIRE(control_hdr.is_object());
    auto const& typ = control_hdr.get_object();
    INFO("control：set_type 设置的 typ 在 sign() 后存活 —— 断言/脚手架可靠");
    auto typ_it = typ.if_contains("typ");
    REQUIRE(typ_it != nullptr);
    REQUIRE(typ_it->as_string() == "JWT");

    // ---- 验证点 ----
    // 已修复：sign() 只更新 alg/typ 两个结构性字段，此前 set_header_claim 家族
    // 写入的 kid/cty/自定义 claim 全部保留在最终 token 的 header 里。
    // （旧实现用 `header_ = { {alg, name} }` 整体覆盖，除 typ 外全部丢失。）
    auto token = httplib::jwt::create()
                     .set_key_id("k1")
                     .set_content_type("at+jwt")
                     .set_header_claim("x-header-tag", boost::json::value("v1"))
                     .sign(httplib::jwt::hs256("secret"));
    auto hdr = httplib::jwt::decode(token).value().get_header();
    REQUIRE(hdr.is_object());
    auto const& obj = hdr.get_object();

    auto kid = obj.if_contains("kid");
    INFO("回归：set_key_id('k1') 设置的 kid 必须出现在最终 token header 中");
    REQUIRE(kid != nullptr);
    REQUIRE(kid->as_string() == "k1");

    auto cty = obj.if_contains("cty");
    INFO("回归：set_content_type('at+jwt') 设置的 cty 必须出现在最终 token header 中");
    REQUIRE(cty != nullptr);
    REQUIRE(cty->as_string() == "at+jwt");

    auto custom = obj.if_contains("x-header-tag");
    INFO("回归：set_header_claim 设置的自定义 header claim 必须出现在最终 token 中");
    REQUIRE(custom != nullptr);
    REQUIRE(custom->as_string() == "v1");
}

// ---------------------------------------------------------------------------
// control（正例回归，本文件应保持全绿）：带算法的 verifier 正常路径不受影响
// ---------------------------------------------------------------------------
TEST_CASE("control: fully configured verifier still accepts a valid token",
          "[p0][jwt][control][do-not-fail]")
{
    auto alg = httplib::jwt::hs256("real-secret");
    auto token = httplib::jwt::create() //
                     .set_issuer("my-app")
                     .set_expires_in(std::chrono::hours { 1 })
                     .sign(alg);
    auto decoded = httplib::jwt::decode(token).value();

    boost::system::error_code ec;
    httplib::jwt::verify(alg).with_issuer("my-app").verify(decoded, ec);
    REQUIRE_FALSE(ec);
}
