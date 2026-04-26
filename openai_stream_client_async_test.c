/*
 * openai_stream_client_async_test.c
 * 
 * Test program demonstrating async chunk iteration with libmill.
 * Shows how to use next_chunk() to iterate chunks asynchronously.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <curl/curl.h>

#include "openai_stream_client.h"
#include "libmill/libmill.h"
#include "llm_parser.h"
static volatile int g_running = 1;
static volatile int g_streaming = 0;  /* Set to 1 when actively streaming */

/* Load .env file into environment variables */
static void load_env_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return;  /* Silently skip if file doesn't exist */
    }
    
    printf("Loading environment from: %s\n", path);
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* Remove trailing newline/carriage return */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        
        /* Skip empty lines and comments */
        if (len == 0 || line[0] == '#') continue;
        
        /* Find the '=' separator */
        char *eq = strchr(line, '=');
        if (!eq) continue;
        
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        
        /* Trim whitespace from key */
        while (*key == ' ' || *key == '\t') key++;
        char *kend = key + strlen(key) - 1;
        while (kend > key && (*kend == ' ' || *kend == '\t')) *kend-- = '\0';
        
        /* Trim whitespace from value (optional) */
        while (*value == ' ' || *value == '\t') value++;
        
        /* Skip if key is empty */
        if (strlen(key) == 0) continue;
        
        /* Set environment variable, overwriting any existing value */
        setenv(key, value, 1);
    }
    
    fclose(fp);
}

void sigint_handler(int sig) {
    if (g_streaming) {
        g_running = 0;
        printf("Canceling\n");
    } else {
        /* At prompt, just print newline and let user continue */
        printf("\n");
    }
}

/* Coroutine: Process chunks as they arrive */
coroutine void chunk_processor(stream_client_t *client) {
    StreamChunk chunk;
    int count = 0;
    int first_token = 0;
    LlmParser* parser = llm_parser_create();
    llm_parser_reset(parser);
    printf("\033[1;34mAssistant:\033[0m ");
    fflush(stdout);
    LlmParserStatus parser_stauts = LLM_PARSER_IDLE;
    while (next_chunk(client, &chunk)) {
        count++;
        LlmParserStatus parser_stauts_c = llm_parser_feed_chunk(parser, &chunk);
        if (parser_stauts_c != parser_stauts)
        {
            printf("%s\n",llm_parser_status_to_str(parser_stauts_c));
            parser_stauts = parser_stauts_c;
        }
        /* Track first token time */
        if (!first_token && (chunk.content || chunk.reasoning_content)) {
            first_token = 1;
        }
        // if (chunk.role != -1) == 0)
        // {
        //     printf("Assitent: \n");
        //     fflush(stdout);
        // }
        /* Print content as it arrives */
        if (chunk.content) {
            printf("%s", chunk.content);
            fflush(stdout);
        }
        
        if (chunk.reasoning_content) {
            printf("%s", chunk.reasoning_content);
            fflush(stdout);
        }
        
        /* Check for finish */
        if (chunk.finish_reason_present && strlen(chunk.finish_reason) > 0) {
            printf("\n\n[Finish reason: %s]\n", chunk.finish_reason);
        }
        
        /* Print usage if present */
        if (chunk.usage_present) {
            printf("[Usage: prompt=%d, completion=%d, cached=%d, total=%d]\n",
                   chunk.prompt_tokens, chunk.completion_tokens, chunk.cached_tokens, chunk.total_tokens);
        }
        
        /* Cleanup chunk memory */
        stream_chunk_cleanup(&chunk);
    }
    
    printf("\n\n[Received %d chunks in %ld ms]\n", count,
           (long)stream_client_get_total_duration(client));
}

/* Coroutine: Monitor for cancellation */
coroutine void cancellation_monitor(stream_client_t *client) {
    while (g_running && 
           (stream_client_get_state(client) == CLIENT_STATE_STREAMING ||
            stream_client_get_state(client) == CLIENT_STATE_CONNECTING)) {
        msleep(now() + 50);
    }
    
    if (!g_running) {
        printf("\n\n[Interrupt detected, cancelling...]\n");
        stream_client_cancel(client);
    }
}
int main(int argc, char *argv[]) {
    /* Load .env file (if exists) to populate environment variables */
    load_env_file(".env");                     /* Try current directory first */
    load_env_file("../.env");                   /* Try parent directory (build/ -> project root) */
    
    signal(SIGINT, sigint_handler);
    const char *model = "kimi-k2.5";
    const char *api_endpoint = "https://api.moonshot.cn/v1/chat/completions";
    const char *log_file = "openai_stream_async.log";
    /* Get API key from environment */
    const char *api_key = getenv("OPENAI_API_KEY");
    if (!api_key) {
        api_key = getenv("MOONSHOT_API_KEY");
    }
    if (!api_key) {
        fprintf(stderr, "Error: Set OPENAI_API_KEY environment variable\n");
        return 1;
    }
    const char *api_url = getenv("OPENAI_BASE_URL");
    if (!api_url) {
        fprintf(stderr, "Warning: Set OPENAI_BASE_URL environment variable\n");
    }
    else{
        char api_endpint_real[500];
        strcpy(api_endpint_real, api_url);
        strcat(api_endpint_real, "/chat/completions");
        api_endpoint = api_endpint_real;
    }
    const char *model_name = getenv("MODEL_NAME");
    if (!model_name) {
        fprintf(stderr, "Warning: Set MODEL_NAME environment variable\n");
    }
    else{
        model = model_name;
    }

    /* Parse arguments */
    int arg_idx = 1;
    while (arg_idx < argc) {
        if (strcmp(argv[arg_idx], "--model") == 0 && arg_idx + 1 < argc) {
            model = argv[arg_idx + 1];
            arg_idx += 2;
        } else if (strcmp(argv[arg_idx], "--endpoint") == 0 && arg_idx + 1 < argc) {
            api_endpoint = argv[arg_idx + 1];
            arg_idx += 2;
        } else if (strcmp(argv[arg_idx], "--log") == 0 && arg_idx + 1 < argc) {
            log_file = argv[arg_idx + 1];
            arg_idx += 2;
        } else {
            arg_idx++;
        }
    }
    
    /* Initialize CURL */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    printf("╔════════════════════════════════════════════════════════╗\n");
    printf("║   OpenAI Stream Client (Async with libmill)            ║\n");
    printf("╠════════════════════════════════════════════════════════╣\n");
    printf("║ Model: %-47s ║\n", model);
    printf("║ Endpoint: %-44s ║\n", api_endpoint);
    printf("║ Log file: %-44s ║\n", log_file);
    printf("║ Press Ctrl+C during streaming to cancel               ║\n");
    printf("║ Type 'quit' to exit                                   ║\n");
    printf("╚════════════════════════════════════════════════════════╝\n\n");
    
    /* Create client */
    stream_client_t *client = stream_client_new(api_key, model, api_endpoint, log_file);
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }
    
    char input[4096];
    
    while (1) {
        printf("\033[1;32mYou:\033[0m ");
        fflush(stdout);
        yield();
        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }
        
        /* Remove newline */
        size_t len = strlen(input);
        if (len > 0 && input[len-1] == '\n') {
            input[len-1] = '\0';
        }
        
        if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
            printf("\nGoodbye!\n");
            break;
        }
        
        if (strlen(input) == 0) {
            continue;
        }
        
        /* Start streaming chat */
        g_streaming = 1;  /* Mark as streaming so Ctrl+C will cancel */
        if (stream_client_start_chat(client, input) != 0) {
            fprintf(stderr, "\033[1;31mFailed to start chat\033[0m\n");
            g_streaming = 0;
            continue;
        }
        
        /* Launch coroutines to process chunks and monitor for cancellation */
        go(chunk_processor(client));
        go(cancellation_monitor(client));
        
        /* Main loop - can do other work here while streaming happens in background */
        while (g_running && 
               (stream_client_get_state(client) == CLIENT_STATE_STREAMING ||
                stream_client_get_state(client) == CLIENT_STATE_CONNECTING)) {
            msleep(now() + 100);
            /* The actual chunk processing happens in chunk_processor coroutine */
            /* Here we could do other async work */
        }
        
        /* Wait for CURL thread and chunk processor to finish */
        stream_client_wait_done(client);
        /* Give chunk_processor coroutine time to exit its fdwait loop */
        msleep(now() + 200);
        
        printf("\n");
        
        /* Reset flags for next iteration */
        g_running = 1;
        g_streaming = 0;
    }
    
    stream_client_free(client);
    curl_global_cleanup();
    
    return 0;
}
