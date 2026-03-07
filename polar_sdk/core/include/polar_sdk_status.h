// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_STATUS_H
#define POLAR_SDK_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POLAR_OK = 0,
    POLAR_ERR_TIMEOUT,
    POLAR_ERR_NOT_CONNECTED,
    POLAR_ERR_PROTOCOL,
    POLAR_ERR_SECURITY,
    POLAR_ERR_UNSUPPORTED,
    POLAR_ERR_OVERFLOW,
    POLAR_ERR_INVALID_ARG,
    POLAR_ERR_STATE,
    POLAR_ERR_BUSY,
    POLAR_ERR_IO,
} polar_status_t;

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_STATUS_H
