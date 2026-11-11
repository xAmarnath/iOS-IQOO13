#pragma once
#include <string.h>
#include <sys/system_properties.h>
#define PROPERTY_VALUE_MAX PROP_VALUE_MAX
#define PROPERTY_KEY_MAX 128
static inline int property_get(const char *key, char *value, const char *fallback)
{
    int length = __system_property_get(key, value);
    if (length || !fallback) return length;
    return (int)strlcpy(value, fallback, PROPERTY_VALUE_MAX);
}
