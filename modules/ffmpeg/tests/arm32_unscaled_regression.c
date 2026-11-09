/* Standalone FFmpeg ARM32 conversion regression. Build against the patched SDK
 * and run on an ARM32-capable Android device. No Godot runtime is required. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/cpu.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

typedef struct Plane {
    uint8_t *allocation;
    uint8_t *data;
    size_t bytes;
    int rows;
    int stride;
    int offset;
} Plane;

static int checks;
static int current_width, current_height, current_variant, current_flags;

static void require(int condition, const char *message) {
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL %s width=%d height=%d variant=%d flags=%d\n", message, current_width, current_height, current_variant, current_flags);
        exit(1);
    }
}

static Plane plane(int row_bytes, int rows, int offset, int skew, int reverse) {
    Plane p = {0};
    int stride = ((row_bytes + 63) & ~63) + 64 + skew;
    p.bytes = (size_t)stride * rows;
    p.allocation = malloc(p.bytes + offset + 128);
    require(p.allocation != NULL, "allocation");
    memset(p.allocation, 0xa5, p.bytes + offset + 128);
    p.offset = offset;
    p.rows = rows;
    p.stride = reverse ? -stride : stride;
    p.data = p.allocation + 64 + offset + (reverse ? (size_t)(rows - 1) * stride : 0);
    return p;
}

static void guards(const Plane *p) {
    for (int i = 0; i < 64 + p->offset; i++) require(p->allocation[i] == 0xa5, "front guard");
    for (int i = 0; i < 64; i++) require(p->allocation[p->bytes + 64 + p->offset + i] == 0xa5, "end guard");
}

static void fill(Plane *p, int width, int height) {
    for (int y = 0; y < height; y++) {
        uint8_t *row = p->data + (ptrdiff_t)y * p->stride;
        for (int x = 0; x < width; x++) {
            row[x * 4] = (x * 37 + y * 11) & 255;
            row[x * 4 + 1] = (x * 13 + y * 53 + 7) & 255;
            row[x * 4 + 2] = (x * 97 + y * 17 + 31) & 255;
            row[x * 4 + 3] = 255;
        }
    }
}

static struct SwsContext *context(int width, int height, int flags) {
    struct SwsContext *ctx = sws_getContext(width, height, AV_PIX_FMT_RGBA, width, height, AV_PIX_FMT_NV12, flags, NULL, NULL, NULL);
    require(ctx != NULL, "context");
    return ctx;
}

static int convert(struct SwsContext *ctx, Plane *src, Plane *y, Plane *uv, int start, int rows) {
    const uint8_t *input[4] = {src->data + (ptrdiff_t)start * src->stride, NULL, NULL, NULL};
    const int src_stride[4] = {src->stride, 0, 0, 0};
    uint8_t *output[4] = {y->data, uv->data, NULL, NULL};
    const int dst_stride[4] = {y->stride, uv->stride, 0, 0};
    return sws_scale(ctx, input, src_stride, start, rows, output, dst_stride);
}

static void compare(const Plane *reference, const Plane *actual, int row_bytes, int tolerance) {
    int max_difference = 0;
    for (int y = 0; y < actual->rows; y++) {
        const uint8_t *a = reference->data + (ptrdiff_t)y * reference->stride;
        const uint8_t *b = actual->data + (ptrdiff_t)y * actual->stride;
        for (int x = 0; x < row_bytes; x++) {
            int difference = abs((int)a[x] - b[x]);
            if (difference > max_difference) max_difference = difference;
        }
    }
    if (max_difference > tolerance) fprintf(stderr, "maximum pixel difference=%d tolerance=%d\n", max_difference, tolerance);
    require(max_difference <= tolerance, "pixel equivalence");
}

int main(void) {
    const int sizes[][2] = {{1, 1}, {2, 3}, {15, 4}, {16, 1}, {16, 2}, {17, 6}, {18, 6}, {31, 32}, {32, 6}, {48, 32}, {70, 50}, {127, 95}, {128, 98}, {1920, 1080}};
    const int cpu = av_get_cpu_flags();
    require(cpu > 0, "physical ARM SIMD CPU features");
    int cases = 0, fallback_cases = 0, sliced_cases = 0, rejected_slices = 0, frame_api_cases = 0, yuv_cases = 0;
    for (int rounding = 0; rounding < 2; rounding++) {
        for (size_t size = 0; size < sizeof(sizes) / sizeof(sizes[0]); size++) {
            int width = sizes[size][0], height = sizes[size][1];
            int flags = SWS_BILINEAR | (rounding ? SWS_ACCURATE_RND : 0);
            current_width = width; current_height = height; current_flags = flags; current_variant = -1;
            fprintf(stderr, "TRACE width=%d height=%d flags=%d reference\n", width, height, flags); fflush(stderr);
            Plane src_ref = plane(width * 4, height, 0, 0, 0);
            Plane y_ref = plane(width, height, 0, 0, 0);
            Plane uv_ref = plane(((width + 1) / 2) * 2, (height + 1) / 2, 0, 0, 0);
            fill(&src_ref, width, height);
            struct SwsContext *reference = context(width, height, flags);
            require(convert(reference, &src_ref, &y_ref, &uv_ref, 0, height) == height, "full reference row count");
            sws_freeContext(reference);
            int eligible = width >= 16 && !(width & 15) && !(height & 1);
            if (!eligible) {
                av_force_cpu_flags(0);
                struct SwsContext *scalar = context(width, height, flags);
                av_force_cpu_flags(cpu);
                Plane cy = plane(width, height, 0, 0, 0);
                Plane cuv = plane(((width + 1) / 2) * 2, (height + 1) / 2, 0, 0, 0);
                require(convert(scalar, &src_ref, &cy, &cuv, 0, height) == height, "fallback reference row count");
                compare(&cy, &y_ref, width, 1);
                compare(&cuv, &uv_ref, ((width + 1) / 2) * 2, 1);
                guards(&cy); guards(&cuv);
                free(cy.allocation); free(cuv.allocation); sws_freeContext(scalar);
                fallback_cases++;
            }
            if (eligible) {
                AVFrame *input = av_frame_alloc(), *output = av_frame_alloc();
                require(input && output, "frame objects");
                input->width = output->width = width;
                input->height = output->height = height;
                input->format = AV_PIX_FMT_RGBA; output->format = AV_PIX_FMT_NV12;
                require(av_frame_get_buffer(input, 32) == 0 && av_frame_get_buffer(output, 32) == 0, "frame buffers");
                for (int row = 0; row < height; row++) memcpy(input->data[0] + row * input->linesize[0], src_ref.data + row * src_ref.stride, width * 4);
                struct SwsContext *threaded = sws_alloc_context();
                require(threaded != NULL, "threaded context");
                threaded->src_w = threaded->dst_w = width;
                threaded->src_h = threaded->dst_h = height;
                threaded->src_format = AV_PIX_FMT_RGBA; threaded->dst_format = AV_PIX_FMT_NV12;
                threaded->flags = flags; threaded->threads = 2;
                require(sws_init_context(threaded, NULL, NULL) == 0, "threaded initialization");
                require(sws_scale_frame(threaded, output, input) == 0, "threaded full frame");
                Plane fy = {.data=output->data[0], .rows=height, .stride=output->linesize[0]};
                Plane fuv = {.data=output->data[1], .rows=height/2, .stride=output->linesize[1]};
                compare(&y_ref, &fy, width, 0); compare(&uv_ref, &fuv, width, 0);
                sws_freeContext(threaded); av_frame_free(&input); av_frame_free(&output);
                frame_api_cases++;
            }
            for (int variant = 0; variant < 4; variant++) {
                current_variant = variant;
                fprintf(stderr, "TRACE width=%d height=%d flags=%d variant=%d\n", width, height, flags, variant); fflush(stderr);
                /* Both optimized and fallback paths accept byte-unaligned
                 * packed RGB input, destination addresses and row strides. */
                int alignment_offset = variant;
                Plane src = plane(width * 4, height, alignment_offset, alignment_offset, variant == 3);
                Plane y = plane(width, height, alignment_offset, alignment_offset, variant == 3);
                Plane uv = plane(((width + 1) / 2) * 2, (height + 1) / 2, alignment_offset, alignment_offset, variant == 3);
                fill(&src, width, height);
                size_t input_bytes = src.bytes + src.offset + 128;
                uint8_t *saved = malloc(input_bytes);
                require(saved != NULL, "input copy");
                memcpy(saved, src.allocation, input_bytes);
                struct SwsContext *ctx = context(width, height, flags);
                require(convert(ctx, &src, &y, &uv, 0, height) == height, "unaligned row count");
                compare(&y_ref, &y, width, 0); compare(&uv_ref, &uv, ((width + 1) / 2) * 2, 0);
                require(memcmp(saved, src.allocation, input_bytes) == 0, "input immutable");
                guards(&src); guards(&y); guards(&uv); sws_freeContext(ctx);
                if (eligible && height >= 4 && variant < 3) {
                    for (int direction = 0; direction < 2; direction++) {
                        memset(y.allocation, 0xa5, y.bytes + y.offset + 128);
                        memset(uv.allocation, 0xa5, uv.bytes + uv.offset + 128);
                        ctx = context(width, height, flags);
                        int start = direction ? height - 2 : 0;
                        int rest_start = direction ? 0 : 2;
                        require(convert(ctx, &src, &y, &uv, start, 2) == 2, "first slice row count");
                        require(convert(ctx, &src, &y, &uv, rest_start, height - 2) == height - 2, "remaining slice row count");
                        compare(&y_ref, &y, width, 0); compare(&uv_ref, &uv, ((width + 1) / 2) * 2, 0);
                        require(memcmp(saved, src.allocation, input_bytes) == 0, "sliced input immutable");
                        guards(&y); guards(&uv); sws_freeContext(ctx); sliced_cases++;
                    }
                }
                if (eligible) {
                    memset(y.allocation, 0xa5, y.bytes + y.offset + 128);
                    memset(uv.allocation, 0xa5, uv.bytes + uv.offset + 128);
                    ctx = context(width, height, flags);
                    require(convert(ctx, &src, &y, &uv, 0, 1) == AVERROR(EINVAL), "incomplete row pair rejected");
                    for (size_t i = 0; i < y.bytes + y.offset + 128; i++) require(y.allocation[i] == 0xa5, "rejected slice leaves luma untouched");
                    for (size_t i = 0; i < uv.bytes + uv.offset + 128; i++) require(uv.allocation[i] == 0xa5, "rejected slice leaves chroma untouched");
                    sws_freeContext(ctx); rejected_slices++;
                }
                free(saved); free(src.allocation); free(y.allocation); free(uv.allocation); cases++;
            }
            guards(&src_ref); guards(&y_ref); guards(&uv_ref);
            free(src_ref.allocation); free(y_ref.allocation); free(uv_ref.allocation);
        }
    }
    const enum AVPixelFormat formats[] = {AV_PIX_FMT_NV12, AV_PIX_FMT_NV21, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P};
    for (int format = 0; format < 8; format++) {
        for (int geometry = 0; geometry < 2; geometry++) {
            int width = geometry ? 48 : 32, height = geometry ? 50 : 6;
            int filter_flags = format < 4 ? SWS_BILINEAR : SWS_POINT;
            current_width=width; current_height=height; current_variant=format; current_flags=filter_flags;
            uint8_t *input[4] = {0}; int input_stride[4] = {0};
            int bytes = av_image_alloc(input, input_stride, width, height, formats[format % 4], 32);
            require(bytes > 0, "YUV input allocation"); memset(input[0], 128, bytes);
            Plane output = plane(width * 4, height, 0, 0, 0);
            Plane reference = plane(width * 4, height, 0, 0, 0);
            int output_stride[4] = {output.stride, 0, 0, 0}; uint8_t *out[4] = {output.data, NULL, NULL, NULL};
            struct SwsContext *ctx = sws_getContext(width, height, formats[format % 4], width, height, AV_PIX_FMT_RGBA, filter_flags, NULL, NULL, NULL);
            require(ctx != NULL, "YUV context");
            require(sws_scale(ctx, (const uint8_t *const *)input, input_stride, 0, height, out, output_stride) == height, "YUV optimized row count");
            sws_freeContext(ctx);
            av_force_cpu_flags(0);
            ctx = sws_getContext(width, height, formats[format % 4], width, height, AV_PIX_FMT_RGBA, filter_flags, NULL, NULL, NULL);
            av_force_cpu_flags(cpu); require(ctx != NULL, "YUV scalar context");
            out[0] = reference.data;
            require(sws_scale(ctx, (const uint8_t *const *)input, input_stride, 0, height, out, output_stride) == height, "YUV scalar row count");
            compare(&reference, &output, width * 4, 1); guards(&output); guards(&reference);
            sws_freeContext(ctx); av_freep(&input[0]); free(output.allocation); free(reference.allocation); yuv_cases++;
        }
    }
    printf("{\"passed\":true,\"full_cases\":%d,\"fallback_geometries\":%d,\"sliced_cases\":%d,\"rejected_odd_slices\":%d,\"frame_api_cases\":%d,\"yuv_cases\":%d,\"checks\":%d,\"cpu_flags\":%d}\n", cases, fallback_cases, sliced_cases, rejected_slices, frame_api_cases, yuv_cases, checks, cpu);
    return 0;
}
