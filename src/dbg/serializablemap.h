#include "_global.h"
#include "threading.h"
#include "module.h"

template<class TValue>
class JSONWrapper
{
public:
    virtual ~JSONWrapper()
    {
    }

    void SetJson(JSON json)
    {
        mJson = json;
    }

    virtual bool Save(const TValue & value) = 0;
    virtual bool Load(TValue & value) = 0;

protected:
    void setString(const char* key, const char* value)
    {
        set(key, json_string(value));
    }

    template<size_t TSize>
    bool getString(const char* key, char(&_Dest)[TSize]) const
    {
        auto str = json_string_value(json_object_get(mJson, key));
        if(str && strlen(str) < TSize)
        {
            strcpy_s(_Dest, str);
            return true;
        }
        return false;
    }

    void setHex(const char* key, duint value)
    {
        set(key, json_hex(value));
    }

    bool getHex(const char* key, duint & value) const
    {
        auto jsonValue = get(key);
        if(!jsonValue)
            return false;
        value = duint(json_hex_value(jsonValue));
        return true;
    }

    void setBool(const char* key, bool value)
    {
        set(key, json_boolean(value));
    }

    bool getBool(const char* key, bool & value) const
    {
        auto jsonValue = get(key);
        if(!jsonValue)
            return false;
        value = json_boolean_value(jsonValue);
        return true;
    }

    template<typename T>
    void setInt(const char* key, T value)
    {
        set(key, json_integer(value));
    }

    template<typename T>
    bool getInt(const char* key, T & value)
    {
        auto jsonValue = get(key);
        if(!jsonValue)
            return false;
        value = T(json_integer_value(jsonValue));
        return true;
    }

    // ReSharper disable once CppMemberFunctionMayBeConst
    void set(const char* key, JSON value)
    {
        json_object_set_new(mJson, key, value);
    }

    JSON get(const char* key) const
    {
        return json_object_get(mJson, key);
    }

    JSON mJson = nullptr;
};

template<SectionLock TLock, class TKey, class TValue, class TMap, class TSerializer>
class SerializableTMap
{
    static_assert(std::is_base_of<JSONWrapper<TValue>, TSerializer>::value, "TSerializer is not derived from JSONWrapper<TValue>");
public:
    using TValuePred = std::function<bool(const TValue & value)>;

    virtual ~SerializableTMap()
    {
    }

    bool Add(const TValue & value)
    {
        EXCLUSIVE_ACQUIRE(TLock);
        return addNoLock(value);
    }

    bool Get(const TKey & key, TValue & value) const
    {
        SHARED_ACQUIRE(TLock);
        auto found = mMap.find(key);
        if(found == mMap.end())
            return false;
        value = found->second;
        return true;
    }

    bool Contains(const TKey & key) const
    {
        SHARED_ACQUIRE(TLock);
        return mMap.count(key) > 0;
    }

    bool Delete(const TKey & key)
    {
        EXCLUSIVE_ACQUIRE(TLock);
        return mMap.erase(key) > 0;
    }

    void DeleteWhere(TValuePred predicate)
    {
        EXCLUSIVE_ACQUIRE(TLock);
        for(auto itr = mMap.begin(); itr != mMap.end();)
        {
            if(predicate(itr->second))
                itr = mMap.erase(itr);
            else
                ++itr;
        }
    }

    bool GetWhere(TValuePred predicate, TValue & value)
    {
        return getWhere(predicate, &value);
    }

    bool GetWhere(TValuePred predicate)
    {
        return getWhere(predicate, nullptr);
    }

    void Clear()
    {
        EXCLUSIVE_ACQUIRE(TLock);
        mMap.clear();
    }

    void CacheSave(JSON root) const
    {
        SHARED_ACQUIRE(TLock);
        auto jsonValues = json_array();
        TSerializer serializer;
        for(const auto & itr : mMap)
        {
            auto jsonValue = json_object();
            serializer.SetJson(jsonValue);
            if(serializer.Save(itr.second))
                json_array_append_new(jsonValues, jsonValue);
            else
                json_decref(jsonValue);
        }
        if(json_array_size(jsonValues))
            json_object_set(root, jsonKey(), jsonValues);
        json_decref(jsonValues);
    }

    void CacheLoad(JSON root, bool clear = true, const char* keyprefix = nullptr)
    {
        if(clear)
            Clear();
        EXCLUSIVE_ACQUIRE(TLock);
        auto jsonValues = json_object_get(root, keyprefix ? (keyprefix + String(jsonKey())).c_str() : jsonKey());
        if(!jsonValues)
            return;
        size_t i;
        JSON jsonValue;
        TSerializer deserializer;
        json_array_foreach(jsonValues, i, jsonValue)
        {
            deserializer.SetJson(jsonValue);
            TValue value;
            if(deserializer.Load(value))
                addNoLock(value);
        }
    }

    void GetList(std::vector<TValue> & values) const
    {
        SHARED_ACQUIRE(TLock);
        values.clear();
        values.reserve(mMap.size());
        for(const auto & itr : mMap)
            values.push_back(itr.second);
    }

    bool Enum(TValue* list, size_t* size) const
    {
        if(!list && !size)
            return false;
        SHARED_ACQUIRE(TLock);
        if(size)
        {
            *size = mMap.size() * sizeof(TValue);
            if(!list)
                return true;
        }
        for(auto & itr : mMap)
        {
            *list = itr.second;
            AdjustValue(*list);
            ++list;
        }
        return true;
    }

    bool GetInfo(const TKey & key, TValue* valuePtr) const
    {
        TValue value;
        if(!Get(key, value))
            return false;
        if(valuePtr)
            *valuePtr = value;
        return true;
    }

    TMap & GetDataUnsafe()
    {
        return mMap;
    }

    virtual void AdjustValue(TValue & value) const = 0;

protected:
    virtual const char* jsonKey() const = 0;
    virtual TKey makeKey(const TValue & value) const = 0;

private:
    TMap mMap;

    bool addNoLock(const TValue & value)
    {
        mMap[makeKey(value)] = value;
        return true;
    }

    bool getWhere(TValuePred predicate, TValue* value)
    {
        SHARED_ACQUIRE(TLock);
        for(const auto & itr : mMap)
        {
            if(!predicate(itr.second))
                continue;
            if(value)
                *value = itr.second;
            return true;
        }
        return false;
    }
};

template<SectionLock TLock, class TKey, class TValue, class TSerializer, class THash = std::hash<TKey>>
using SerializableUnorderedMap = SerializableTMap<TLock, TKey, TValue, std::unordered_map<TKey, TValue, THash>, TSerializer>;

template<SectionLock TLock, class TKey, class TValue, class TSerializer, class TCompare = std::less<TKey>>
using SerializableMap = SerializableTMap<TLock, TKey, TValue, std::map<TKey, TValue, TCompare>, TSerializer>;

template<SectionLock TLock, class TValue, class TSerializer>
struct SerializableModuleRangeMap : SerializableMap<TLock, ModuleRange, TValue, TSerializer, ModuleRangeCompare>
{
    static ModuleRange VaKey(duint start, duint end)
    {
        auto moduleBase = ModBaseFromAddr(start);
        return ModuleRange(ModHashFromAddr(moduleBase), Range(start - moduleBase, end - moduleBase));
    }
};

template<SectionLock TLock, class TValue, class TSerializer>
struct SerializableModuleHashMap : SerializableUnorderedMap<TLock, duint, TValue, TSerializer>
{
    static duint VaKey(duint addr)
    {
        return ModHashFromAddr(addr);
    }

    void DeleteRange(duint start, duint end, std::function<bool(duint start, duint end, const TValue & value)> inRange)
    {
        // Are all comments going to be deleted?
        // 0x00000000 - 0xFFFFFFFF
        if(start == 0 && end == ~0)
        {
            Clear();
        }
        else
        {
            // Make sure 'Start' and 'End' reference the same module
            duint moduleBase = ModBaseFromAddr(start);

            if(moduleBase != ModBaseFromAddr(end))
                return;

            // Virtual -> relative offset
            start -= moduleBase;
            end -= moduleBase;

            DeleteWhere([start, end, inRange](const TValue & value)
            {
                return inRange(start, end, value);
            });
        }
    }
};