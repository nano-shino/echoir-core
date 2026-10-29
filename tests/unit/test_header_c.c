/*
 * C99 check of the public header. This file is built as C99 with no C++ runtime
 * (tests/CMakeLists.txt), so it proves echoir.h is valid plain C and links against echoir.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "echoir/echoir.h"

static int g_failures = 0;

#define EXPECT(cond)                                                                  \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "%s:%d: EXPECT failed: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                             \
        }                                                                             \
    } while (0)

int main(void) {
    const char* version = echoir_version();
    echoir_engine_params ep;
    echoir_session_params sp;
    echoir_event ev;

    EXPECT(version != NULL && version[0] != '\0');

    echoir_engine_params_init(&ep);
    EXPECT(ep.struct_size == sizeof(echoir_engine_params));
    EXPECT(ep.model_path == NULL);

    echoir_session_params_init(&sp);
    EXPECT(sp.struct_size == sizeof(echoir_session_params));
    EXPECT(strcmp(sp.language, "auto") == 0);
    EXPECT(sp.min_chunk_ms == 500);
    EXPECT(sp.max_segment_ms == 10000);

    /* Engine and session create need a model; with no paths they return NULL. The fake build
       loads no model, so it has no such check. A NULL session is rejected before any session
       state is touched. */
#ifndef ECHOIR_FAKE_BACKEND
    EXPECT(echoir_engine_create(&ep) == NULL);
#endif
    EXPECT(echoir_session_create(NULL, &sp, NULL, NULL) == NULL);
    EXPECT(echoir_session_poll(NULL, &ev) == ECHOIR_ERR_INVALID_ARG);
    EXPECT(echoir_session_start(NULL) == ECHOIR_ERR_INVALID_ARG);
    EXPECT(strstr(echoir_last_error(), "echoir_session_start") != NULL);

    /* A session params struct from a newer header is refused, and so is an unknown policy. Both
       are checked before the engine, so they fail here with no engine. */
    echoir_session_params_init(&sp);
    sp.struct_size = (uint32_t)(sizeof(echoir_session_params) + 1);
    EXPECT(echoir_session_create(NULL, &sp, NULL, NULL) == NULL);
    echoir_session_params_init(&sp);
    sp.stream_policy = 99;
    EXPECT(echoir_session_create(NULL, &sp, NULL, NULL) == NULL);
    /* A struct that ends at emit_partials is not a struct error. It gets past the params, and the
       create fails only for the missing engine. */
    echoir_session_params_init(&sp);
    sp.struct_size = (uint32_t)offsetof(echoir_session_params, stream_policy);
    EXPECT(echoir_session_create(NULL, &sp, NULL, NULL) == NULL);
    EXPECT(strstr(echoir_last_error(), "struct_size") == NULL);

    /* Model params: the defaults, and a create with no model id fails with NULL. The catalogue
       JSON works in every build, with or without the backends. */
    echoir_model_params mp;
    echoir_model_params_init(&mp);
    EXPECT(mp.struct_size == sizeof(echoir_model_params));
    EXPECT(mp.registry_json == NULL);
    EXPECT(mp.use_gpu == 1);
    EXPECT(echoir_engine_create_from_model(&mp) == NULL);
    EXPECT(echoir_models_json(NULL, NULL) != NULL);
    EXPECT(echoir_models_json("not json", NULL) == NULL);

    if (g_failures != 0) {
        fprintf(stderr, "c_header: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("c_header ok (echoir %s)\n", version);
    return 0;
}
