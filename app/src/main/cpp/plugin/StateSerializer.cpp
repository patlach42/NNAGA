/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 * Guitar RackCraft is free software under the GNU General Public License v3.
 */
#include "StateSerializer.h"
#include <sstream>
#include <cstdio>

namespace guitarrackcraft {

static const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t* data, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        result += kBase64Chars[(n >> 18) & 0x3F];
        result += kBase64Chars[(n >> 12) & 0x3F];
        result += (i + 1 < len) ? kBase64Chars[(n >> 6) & 0x3F] : '=';
        result += (i + 2 < len) ? kBase64Chars[n & 0x3F] : '=';
    }
    return result;
}

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c))); out += buf;
                } else out += c;
        }
    }
    return out;
}

static bool isStringType(const std::string& typeUri) {
    return typeUri == "http://lv2plug.in/ns/ext/atom#String" ||
           typeUri == "http://lv2plug.in/ns/ext/atom#Path" ||
           typeUri == "http://lv2plug.in/ns/ext/atom#URI";
}

static void serializePlugins(std::ostringstream& os, const PluginChain::ChainState& state, const char* indent) {
    os << "[";
    for (size_t pi = 0; pi < state.plugins.size(); ++pi) {
        const auto& ps = state.plugins[pi];
        if (pi > 0) os << ",";
        os << "\n" << indent << "  {\n";
        os << indent << "    \"uri\": \"" << jsonEscape(ps.pluginUri) << "\",\n";
        os << indent << "    \"format\": \"" << jsonEscape(ps.format) << "\",\n";
        os << indent << "    \"controlPorts\": [";
        for (size_t ci = 0; ci < ps.controlPortValues.size(); ++ci) {
            if (ci > 0) os << ", ";
            os << "{\"index\": " << ps.controlPortValues[ci].first << ", \"value\": " << ps.controlPortValues[ci].second << "}";
        }
        os << "],\n" << indent << "    \"stateProperties\": [";
        for (size_t si = 0; si < ps.properties.size(); ++si) {
            const auto& prop = ps.properties[si];
            if (si > 0) os << ",";
            os << "\n" << indent << "      {\n";
            os << indent << "        \"key\": \"" << jsonEscape(prop.keyUri) << "\",\n";
            os << indent << "        \"type\": \"" << jsonEscape(prop.typeUri) << "\",\n";
            os << indent << "        \"flags\": " << prop.flags << ",\n";
            if (isStringType(prop.typeUri) && !prop.value.empty()) {
                size_t len = prop.value.size(); if (len > 0 && prop.value[len - 1] == 0) --len;
                os << indent << "        \"value\": \"" << jsonEscape(std::string(reinterpret_cast<const char*>(prop.value.data()), len)) << "\"\n";
            } else if (!prop.value.empty()) {
                os << indent << "        \"encoding\": \"base64\",\n" << indent << "        \"value\": \"" << base64Encode(prop.value.data(), prop.value.size()) << "\"\n";
            } else os << indent << "        \"value\": \"\"\n";
            os << indent << "      }";
        }
        if (!ps.properties.empty()) os << "\n" << indent << "    ";
        os << "]\n" << indent << "  }";
    }
    if (!state.plugins.empty()) os << "\n" << indent;
    os << "]";
}

static void serializeChainObject(std::ostringstream& os, const PluginChain::ChainState& chain, float volume, bool armed, bool includeControls) {
    os << "{\n";
    if (includeControls) os << "  \"volume\": " << volume << ",\n  \"inputArmed\": " << (armed ? "true" : "false") << ",\n";
    os << "  \"plugins\": ";
    serializePlugins(os, chain, "  ");
    os << "\n}";
}

std::string serializeRackStateToJson(const RackGraph::State& state) {
    std::ostringstream os;
    os << "{\n  \"version\": 2,\n  \"tracks\": [";
    for (size_t i = 0; i < state.tracks.size(); ++i) {
        if (i > 0) os << ",";
        os << "\n    ";
        serializeChainObject(os, state.tracks[i].chain, state.tracks[i].volume, state.tracks[i].inputArmed, true);
    }
    if (!state.tracks.empty()) os << "\n  ";
    os << "],\n  \"master\": ";
    serializeChainObject(os, state.master, 0.0f, false, false);
    os << "\n}\n";
    return os.str();
}

} // namespace guitarrackcraft
