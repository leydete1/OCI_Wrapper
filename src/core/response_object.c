#include <stdlib.h>
#include <string.h>

#include "response_object.h"

void response_object_init(response_object_t *resp)
{
    memset(resp, 0, sizeof(*resp));
    resp->status = RESPONSE_STATUS_ERROR;   /* safe default - a caller that
                                                forgets to set status ends
                                                up ERROR, not a silently
                                                false PASS                */
    strcpy(resp->audit_id,   "-");
    strcpy(resp->operation,  "-");
    strcpy(resp->error_code, "-");
    strcpy(resp->error_text, "-");
    resp->response_body = NULL;
    resp->is_json       = 0;
}

void response_object_free(response_object_t *resp)
{
    if (resp->response_body)
        free(resp->response_body);
    response_object_init(resp);
}
