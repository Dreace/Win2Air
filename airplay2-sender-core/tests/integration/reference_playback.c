#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include "temporary_file.h"

static int find_range(FILE *expected, FILE *received, int report)
{
    unsigned char *input = NULL, *output = NULL;
    long input_size = 0, output_size = 0;
    size_t found = 0, offset = 0;
    size_t longest = 0, best_offset = 0;
    size_t nonzero_output = 0, first_nonzero = 0;
    int nonzero = 0, result = 1;
    if (fseek(expected,0,SEEK_END) || fseek(received,0,SEEK_END)) goto done;
    input_size = ftell(expected); output_size = ftell(received);
    if (input_size <= 0 || output_size < input_size || output_size > 67108864 || input_size % 4 || output_size % 4) goto done;
    rewind(expected); rewind(received);
    input = malloc((size_t)input_size); output = malloc((size_t)output_size);
    if (!input || !output || fread(input,1,(size_t)input_size,expected) != (size_t)input_size ||
        fread(output,1,(size_t)output_size,received) != (size_t)output_size) goto done;
    for (long i = 0; i < input_size; ++i) nonzero |= input[i] != 0;
    for (long i = 0; i < output_size; ++i) if (output[i]) {
        if (!nonzero_output) first_nonzero = (size_t)i;
        ++nonzero_output;
    }
    if (!nonzero) goto done;
    for (size_t i = 0; i <= (size_t)(output_size-input_size); i += 4) {
        if (memcmp(input,output+i,4) == 0) {
            size_t equal = 0;
            while (equal < (size_t)input_size && input[equal] == output[i+equal]) ++equal;
            if (equal > longest) { longest = equal; best_offset = i; }
            if (equal == (size_t)input_size) { offset = i; ++found; }
        }
    }
    if (found != 1) goto done;
    for (size_t i = 0; i < offset; ++i) if (output[i]) goto done;
    for (size_t i = offset+(size_t)input_size; i < (size_t)output_size; ++i) if (output[i]) goto done;
    if (report) printf("{\"summary_id\":\"receiver-complete-input-range-equal\",\"offset_frames\":%llu,\"frames\":%llu,\"trailing_silent_frames\":%llu}\n",
        (unsigned long long)(offset/4),(unsigned long long)(input_size/4),
        (unsigned long long)(((size_t)output_size-offset-(size_t)input_size)/4));
    result = 0;
done:
    if (result && report && input && output && nonzero_output && first_nonzero + 64 < (size_t)output_size) {
        size_t frame = first_nonzero / 4 * 4;
        for (size_t i = 0; i + 64 <= (size_t)input_size; i += 4)
            if (memcmp(input+i, output+frame, 64) == 0) {
                fprintf(stderr, "operation=diagnose-pcm first_output_frame=%zu corresponding_input_frame=%zu\n", frame/4, i/4);
                break;
            }
    }
    free(input); free(output);
    if (result && report) fprintf(stderr,"operation=locate-pcm stage=playback reason=complete-unique-input-with-silent-padding-required input_bytes=%ld output_bytes=%ld complete_matches=%zu longest_prefix_bytes=%zu offset_frames=%zu nonzero_output_bytes=%zu first_nonzero_frame=%zu\n", input_size, output_size, found, longest, best_offset/4, nonzero_output, first_nonzero/4);
    return result;
}

static int range_case(const unsigned char *input_data, size_t input_size,
                      const unsigned char *output_data, size_t output_size, int expected_result)
{
    FILE *input = temporary_file(), *output = temporary_file();
    int result = 0;
    if (!input || !output) goto done;
    if (fwrite(input_data,1,input_size,input) != input_size || fwrite(output_data,1,output_size,output) != output_size) goto done;
    rewind(input); rewind(output);
    result = find_range(input,output,0) == expected_result;
done:
    if (input) fclose(input);
    if (output) fclose(output);
    return result;
}

static int range_self_test(void)
{
    const unsigned char input[] = {1,2,3,4,5,6,7,8};
    const unsigned char padded[] = {0,0,0,0,1,2,3,4,5,6,7,8,0,0,0,0};
    const unsigned char altered[] = {0,0,0,0,1,2,3,4,5,6,7,9,0,0,0,0};
    const unsigned char noisy[] = {9,0,0,0,1,2,3,4,5,6,7,8,0,0,0,0};
    const unsigned char repeated[] = {1,2,3,4,5,6,7,8,1,2,3,4,5,6,7,8};
    const unsigned char silence[] = {0,0,0,0};
    int ok = range_case(input,sizeof input,padded,sizeof padded,0) &&
             range_case(input,sizeof input,input,sizeof input,0) &&
             range_case(input,sizeof input,altered,sizeof altered,1) &&
             range_case(input,sizeof input,noisy,sizeof noisy,1) &&
             range_case(input,sizeof input,repeated,sizeof repeated,1) &&
             range_case(input,sizeof input,input,sizeof input-1,1) &&
             range_case(input,0,input,sizeof input,1) &&
             range_case(silence,sizeof silence,silence,sizeof silence,1);
    if (!ok) fprintf(stderr,"operation=verify-pcm-range stage=self-test reason=unexpected-range-result\n");
    return ok ? 0 : 1;
}

static int compare_pcm(FILE *expected, FILE *received, uint64_t *frames)
{
    unsigned char input[4096], output[4096];
    size_t read_input, read_output;
    uint64_t bytes = 0;
    int nonzero = 0;
    do {
        read_input = fread(input, 1, sizeof input, expected);
        read_output = fread(output, 1, sizeof output, received);
        if (ferror(expected) || ferror(received)) return 2;
        if (read_input != read_output || memcmp(input, output, read_input) != 0) return 1;
        for (size_t i = 0; i < read_input; ++i) nonzero |= input[i] != 0;
        bytes += read_input;
    } while (read_input != 0);
    if (bytes == 0 || bytes % 4 != 0 || !nonzero) return 2;
    *frames = bytes / 4;
    return 0;
}

static int self_test(void)
{
    const unsigned char stimulus[] = {1, 2, 3, 4, 5, 6, 7, 8};
    FILE *input = temporary_file(), *output = temporary_file();
    uint64_t frames;
    int ok;
    if (!input || !output) {
        if (input) fclose(input);
        if (output) fclose(output);
        return 1;
    }
    ok = fwrite(stimulus, 1, sizeof stimulus, input) == sizeof stimulus &&
         fwrite(stimulus, 1, sizeof stimulus, output) == sizeof stimulus;
    rewind(input); rewind(output);
    ok = ok && compare_pcm(input, output, &frames) == 0 && frames == 2;
    rewind(output); fputc(9, output); fflush(output);
    rewind(input); rewind(output);
    ok = ok && compare_pcm(input, output, &frames) == 1;
    fclose(output); output = temporary_file();
    if (!output) { fclose(input); return 1; }
    rewind(input);
    ok = ok && compare_pcm(input, output, &frames) == 1;
    fclose(input); input = temporary_file();
    if (!input) { fclose(output); return 1; }
    ok = ok && compare_pcm(input, output, &frames) == 2;
    fclose(input); fclose(output);
    input = temporary_file(); output = temporary_file();
    if (!input || !output) {
        if (input) fclose(input);
        if (output) fclose(output);
        fprintf(stderr, "operation=create-verifier-fixture stage=self-test code=%d\n", errno);
        return 1;
    }
    ok = ok && fwrite(stimulus, 1, 3, input) == 3 && fwrite(stimulus, 1, 3, output) == 3;
    rewind(input); rewind(output);
    ok = ok && compare_pcm(input, output, &frames) == 2;
    rewind(input); rewind(output);
    for (unsigned i = 0; i < 4; ++i) {
        ok = ok && fputc(0, input) != EOF && fputc(0, output) != EOF;
    }
    rewind(input); rewind(output);
    ok = ok && compare_pcm(input, output, &frames) == 2;
    fclose(input); fclose(output);
    if (!ok) fprintf(stderr, "operation=verify-pcm-rejection stage=self-test reason=unexpected-comparison-result\n");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    FILE *input, *output;
    uint64_t frames = 0;
    int result;
    if (argc == 3 && strcmp(argv[1], "--sender-diagnostic") == 0) {
        FILE *log = fopen(argv[2], "rb");
        if (!log) return 2;
        char line[1024];
        while (fgets(line, sizeof(line), log))
            if (!strncmp(line, "operation=", 10) || !strncmp(line, "state=", 6) ||
                !strncmp(line, "{\"sender_completed\":", 20)) fputs(line, stderr);
        int failed = ferror(log);
        fclose(log);
        return failed ? 2 : 0;
    }
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) return self_test();
    if (argc == 2 && strcmp(argv[1], "--range-self-test") == 0) return range_self_test();
    int ranged = argc == 5 && strcmp(argv[1],"--find-range") == 0;
    if (ranged) { --argc; ++argv; }
    if (argc != 4 || strcmp(argv[3], "s16le-stereo-44100") != 0 || strcmp(argv[1], argv[2]) == 0) {
        fprintf(stderr, "operation=validate-playback stage=input reason=expected-distinct-pcm-files-and-declared-format\n");
        return 2;
    }
    input = fopen(argv[1], "rb");
    if (!input) { fprintf(stderr, "operation=read-input stage=playback code=%d\n", errno); return 2; }
    output = fopen(argv[2], "rb");
    if (!output) {
        fprintf(stderr, "operation=read-receiver-output stage=playback code=%d\n", errno);
        fclose(input); return 2;
    }
    if (ranged) {
        result = find_range(input,output,1);
        fclose(input); fclose(output);
        return result;
    }
    result = compare_pcm(input, output, &frames);
    fclose(input); fclose(output);
    if (result != 0) {
        fprintf(stderr, "operation=compare-pcm stage=playback code=%d reason=missing-invalid-or-different-audio\n", result);
        return 1;
    }
    printf("{\"summary_id\":\"receiver-samples-equal\",\"frames\":%llu}\n", (unsigned long long)frames);
    return 0;
}
