/*
 * Json.h — 轻量手写 JSON 解析/序列化模块
 *
 * 动机：工程约束不引入第三方库，且配置文件、TCP 告警报文都需要 JSON。
 * 支持：null / bool / number / string / array / object 的解析与序列化。
 * 实现：递归下降解析器，带完整错误信息；序列化带字符串转义。
 *
 * 用法示例：
 *   Json obj(Json::Object);
 *   obj["type"] = "alarm";
 *   obj["stream"] = 0;
 *   std::string s = obj.dump();            // 序列化
 *   Json parsed;
 *   std::string err;
 *   if (Json::parse(s, parsed, err)) { ... }  // 解析
 */
#ifndef MULTI_STREAM_JSON_H
#define MULTI_STREAM_JSON_H

#include <cstdint>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace msad {

class Json {
public:
    /* JSON 数据类型 */
    enum class Type { Null, Bool, Number, String, Array, Object };

    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() : type_(Type::Null) {}
    explicit Json(bool v)  : type_(Type::Bool), bool_(v) {}
    explicit Json(double v): type_(Type::Number), number_(v) {}
    explicit Json(int v)   : type_(Type::Number), number_(static_cast<double>(v)) {}
    explicit Json(const std::string& v) : type_(Type::String), string_(v) {}
    explicit Json(const char* v)        : type_(Type::String), string_(v) {}
    explicit Json(Type t) : type_(t) {
        if (t == Type::Array)  array_ = std::make_shared<Array>();
        if (t == Type::Object) object_ = std::make_shared<Object>();
    }

    /* ---- 赋值重载（便于直接写入：j["k"] = "v" / 5 / true） ---- */
    Json& operator=(const char* v)  { type_ = Type::String; string_ = v; return *this; }
    Json& operator=(const std::string& v) { type_ = Type::String; string_ = v; return *this; }
    Json& operator=(bool v)         { type_ = Type::Bool;   bool_ = v; return *this; }
    Json& operator=(double v)       { type_ = Type::Number; number_ = v; return *this; }
    Json& operator=(int v)          { type_ = Type::Number; number_ = static_cast<double>(v); return *this; }

    Type type() const { return type_; }

    /* ---- 类型查询 ---- */
    bool isNull()   const { return type_ == Type::Null; }
    bool isBool()   const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray()  const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    /* ---- 取值（带默认值，便于配置容错） ---- */
    bool asBool(bool def = false) const {
        return isBool() ? bool_ : def;
    }
    double asNumber(double def = 0.0) const {
        return isNumber() ? number_ : def;
    }
    int asInt(int def = 0) const {
        return isNumber() && std::isfinite(number_) && number_ >= std::numeric_limits<int>::min() &&
               number_ <= std::numeric_limits<int>::max() ? static_cast<int>(number_) : def;
    }
    std::string asString(const std::string& def = "") const {
        return isString() ? string_ : def;
    }

    Array& array() {
        if (!array_) array_ = std::make_shared<Array>();
        return *array_;
    }
    Object& object() {
        if (!object_) object_ = std::make_shared<Object>();
        return *object_;
    }
    const Array& array() const {
        static const Array kEmpty;
        return array_ ? *array_ : kEmpty;
    }
    const Object& object() const {
        static const Object kEmpty;
        return object_ ? *object_ : kEmpty;
    }

    /* 对象下标访问（自动转为 Object 类型） */
    Json& operator[](const std::string& key) {
        if (type_ != Type::Object) {
            type_ = Type::Object;
            object_ = std::make_shared<Object>();
        }
        return (*object_)[key];
    }
    const Json& operator[](const std::string& key) const {
        static const Json kNull;
        if (type_ != Type::Object) return kNull;
        auto it = object_->find(key);
        return it == object_->end() ? kNull : it->second;
    }

    /* 数组下标访问（非 const：自动转为数组类型） */
    Json& operator[](size_t idx) {
        if (type_ != Type::Array) {
            type_ = Type::Array;
            array_ = std::make_shared<Array>();
        }
        if (idx >= array_->size()) array_->resize(idx + 1);
        return (*array_)[idx];
    }

    /* 数组下标访问（const 版本） */
    const Json& operator[](size_t idx) const {
        static const Json kNull;
        if (type_ != Type::Array || !array_ || idx >= array_->size()) return kNull;
        return (*array_)[idx];
    }

    /* 对象是否含指定键 */
    bool has(const std::string& key) const {
        return type_ == Type::Object && object_->count(key) > 0;
    }

    /* 数组长度 */
    size_t size() const {
        if (type_ == Type::Array && array_) return array_->size();
        if (type_ == Type::Object && object_) return object_->size();
        return 0;
    }

    /* ---- 序列化：转成紧凑 JSON 字符串 ---- */
    std::string dump() const;

    /* ---- 解析：成功返回 true，失败返回 false 并填充 err ---- */
    static bool parse(const std::string& text, Json& out, std::string& err);

    /* 内部解析状态（递归下降）；作为公共类型供本模块实现使用 */
    struct Parser {
        const std::string& s;
        size_t pos = 0;
        std::string err;
        size_t depth = 0;
        bool fail(const std::string& msg) { err = msg; return false; }
    };
    static bool parseValue(Parser& p, Json& out);

private:

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::shared_ptr<Array> array_;    /* 用 shared_ptr 保持拷贝语义简单 */
    std::shared_ptr<Object> object_;
};

} // namespace msad

#endif // MULTI_STREAM_JSON_H
