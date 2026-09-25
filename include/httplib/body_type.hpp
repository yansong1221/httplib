#pragma once

namespace httplib
{
    /// 已物化 body 的类型（server::request / client::request / client::response 的 type() 返回值）。
    ///
    /// 与 body 的实际传输格式（Content-Type）对应：读取时按 Content-Type 选择 sink，
    /// 读完后类型固定；未读取时为 @ref none。
    enum class body_type
    {
        none,         ///< 尚未读取
        empty,        ///< 显式空 body
        string,       ///< 文本
        json,         ///< JSON
        query_params, ///< application/x-www-form-urlencoded
        form_data,    ///< multipart/form-data
        file,         ///< 文件型（只写来源）
    };
} // namespace httplib
