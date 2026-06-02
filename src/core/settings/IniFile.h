#pragma once

#include <map>
#include <string>

// A tiny, toolkit-neutral INI reader/writer with the same group/key shape the
// app previously got from Glib::KeyFile (sections in [brackets], key=value
// lines, '#' comments). Replacing KeyFile keeps settings handling out of the
// toolkit layer so both backends share it.
//
// Values are stored as strings; the typed accessors parse on demand and fall
// back to a default when the key is missing or malformed. Booleans serialise as
// "true"/"false" to stay compatible with existing layout.ini files.
class IniFile {
public:
    bool loadFromFile(const std::string& path);  // false if the file can't be read
    std::string toString() const;

    bool hasGroup(const std::string& group) const;
    bool hasKey(const std::string& group, const std::string& key) const;

    std::string getString(const std::string& group, const std::string& key,
                          const std::string& def = "") const;
    int  getInt(const std::string& group, const std::string& key, int def = 0) const;
    bool getBool(const std::string& group, const std::string& key, bool def = false) const;

    void setString(const std::string& group, const std::string& key,
                   const std::string& value);
    void setInt(const std::string& group, const std::string& key, int value);
    void setBool(const std::string& group, const std::string& key, bool value);

private:
    // group -> (key -> value); std::map gives deterministic output ordering.
    std::map<std::string, std::map<std::string, std::string>> data_;
};
