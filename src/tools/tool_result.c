#include "tools/tool_result.h"

#include <stdlib.h>
#include <string.h>

json_t *mcp_tool_result_text(const char *text, bool is_error)
{
    json_t *result = json_object();
    json_t *content = json_array();
    json_t *entry = json_object();

    if (!result || !content || !entry)
        goto fail;

    if (json_object_set_new(entry, "type", json_string("text")) != 0)
        goto fail;
    if (json_object_set_new(entry, "text", json_stringn_nocheck(text ? text : "", text ? strlen(text) : 0)) != 0)
        goto fail;
    if (json_array_append_new(content, entry) != 0)
        goto fail_detach_entry;
    entry = NULL;
    if (json_object_set_new(result, "content", content) != 0)
        goto fail_detach_content;
    content = NULL;
    if (json_object_set_new(result, "isError", json_boolean(is_error)) != 0)
        goto fail;

    return result;

fail_detach_entry:
    json_decref(entry);
    entry = NULL;
fail_detach_content:
    json_decref(content);
    content = NULL;
fail:
    json_decref(entry);
    json_decref(content);
    json_decref(result);
    return json_pack("{s:[{s:s,s:s}],s:b}",
                     "content",
                     "type",
                     "text",
                     "text",
                     "Internal result encoding failure.",
                     "isError",
                     true);
}

json_t *mcp_tool_result_json_text(json_t *value, bool is_error)
{
    char *dumped = json_dumps(value, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_t *result;

    if (!dumped)
        return mcp_tool_result_text("{}", is_error);

    result = mcp_tool_result_text(dumped, is_error);
    free(dumped);
    return result;
}
