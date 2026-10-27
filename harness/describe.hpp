// SPDX-License-Identifier: MIT
//
// C++26 static reflection helpers (P2996, P1306 expansion statements).
//
// Instead of hand-maintaining a serialiser per struct (and forgetting to
// update it when a field is added), these walk an aggregate's members at
// compile time:
//
//   describe(spsc::Tuning{})  -> "padding=128 cache_remote_index=true signal=slot_sequence"
//   to_json(machine_info)     -> {"cpu": "...", "os": "...", ...}
//
// Requires -freflection on GCC 16.
#pragma once

#include <concepts>
#include <meta>
#include <string>
#include <string_view>
#include <type_traits>

namespace harness {

/// Name of an enumerator, e.g. enum_name(Signal::index) == "index".
template <class E>
  requires std::is_enum_v<E>
constexpr std::string_view enum_name(E value) {
  template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^E))) {
    if (value == [:e:]) return std::meta::identifier_of(e);
  }
  return "?";
}

namespace detail {

inline void append_json_string(std::string& out, std::string_view s) {
  out += '"';
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  out += '"';
}

template <class V>
void append_value(std::string& out, const V& v, bool quote_strings) {
  if constexpr (std::same_as<V, bool>) {
    out += v ? "true" : "false";
  } else if constexpr (std::is_enum_v<V>) {
    quote_strings ? append_json_string(out, enum_name(v)) : void(out += enum_name(v));
  } else if constexpr (std::convertible_to<const V&, std::string_view>) {
    quote_strings ? append_json_string(out, v) : void(out += std::string_view(v));
  } else {
    static_assert(std::is_arithmetic_v<V>, "describe/to_json: unsupported member type");
    out += std::to_string(v);
  }
}

consteval auto members_of(std::meta::info type) {
  return std::define_static_array(
      std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked()));
}

}  // namespace detail

/// "field=value field=value ..." for every data member of an aggregate.
template <class S>
std::string describe(const S& s) {
  std::string out;
  template for (constexpr auto m : detail::members_of(^^S)) {
    if (!out.empty()) out += ' ';
    out += std::meta::identifier_of(m);
    out += '=';
    detail::append_value(out, s.[:m:], false);
  }
  return out;
}

/// Flat, pretty-printed JSON object with one key per data member.
template <class S>
std::string to_json(const S& s) {
  std::string out = "{\n";
  bool first = true;
  template for (constexpr auto m : detail::members_of(^^S)) {
    if (!first) out += ",\n";
    first = false;
    out += "  ";
    detail::append_json_string(out, std::meta::identifier_of(m));
    out += ": ";
    detail::append_value(out, s.[:m:], true);
  }
  out += "\n}\n";
  return out;
}

}  // namespace harness
