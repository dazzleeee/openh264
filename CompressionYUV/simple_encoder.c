#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 包含 OpenH264 (Wels) API 头文件
// 你需要确保这个路径在你的 include path 中
#include "wels/codec_api.h"

// --- 你的视频参数 ---
#define INPUT_FILE  "webcam.yuv"
#define OUTPUT_FILE "webcam.264"
#define WIDTH       1280
#define HEIGHT      720
#define FRAME_RATE  30
#define BITRATE     1500000 // 1.5 Mbps

// 计算 YUV 4:2:0 平面的大小
#define Y_SIZE      (WIDTH * HEIGHT)
#define UV_SIZE     (Y_SIZE / 4)
#define FRAME_SIZE  (Y_SIZE + UV_SIZE + UV_SIZE)

int main() {
    ISVCEncoder *encoder = NULL;
    int rv;

    // 1. 创建编码器实例
    rv = WelsCreateSVCEncoder(&encoder);
    if (rv != 0) {
        fprintf(stderr, "WelsCreateSVCEncoder failed. rv = %d\n", rv);
        return -1;
    }

    // 2. 设置编码参数 (使用 SEncParamExt 以获得更精细控制)
    SEncParamExt param;
    memset(&param, 0, sizeof(SEncParamExt));
    
    // 从编码器获取默认参数
    ((*encoder)->GetDefaultParams)(encoder, &param);


    // --- 覆盖关键参数 ---
    param.iUsageType = CAMERA_VIDEO_REAL_TIME; // 使用场景 (摄像头实时视频)
    param.iPicWidth = WIDTH;
    param.iPicHeight = HEIGHT;
    param.fMaxFrameRate = (float)FRAME_RATE;

    // 速率控制 (RC)
    param.iRCMode = RC_BITRATE_MODE; // 码率控制模式
    param.iTargetBitrate = BITRATE;
    param.iMaxBitrate = BITRATE * 1.5; // 峰值码率

    // 输入YUV格式
    // param.iInputColorFormat = videoFormatI420; // 对应 yuv420p 
    // 输入YUV格式 (iInputCsp) 不在 SEncParamExt 中。
    // 我们将在下面的 SSourcePicture (pic.iFormat) 中指定它。
    
    // 配置空间层 (必须设置，否则 InitExt 会失败)
    // 对于单层编码，配置第0层
    param.sSpatialLayers[0].iVideoWidth = param.iPicWidth;
    param.sSpatialLayers[0].iVideoHeight = param.iPicHeight;
    param.sSpatialLayers[0].fFrameRate = param.fMaxFrameRate;
    param.sSpatialLayers[0].iSpatialBitrate = param.iTargetBitrate;
    param.sSpatialLayers[0].iMaxSpatialBitrate = param.iMaxBitrate;
    param.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_SINGLE_SLICE; // 简单的单 slice 模式
    
    // 3. 初始化编码器
    rv = ((*encoder)->InitializeExt)(encoder, &param);
    if (rv != 0) {
        fprintf(stderr, "Encoder InitializeExt failed. rv = %d\n", rv);
        WelsDestroySVCEncoder(encoder);
        return -1;
    }

    // 4. 准备文件 I/O
    FILE* f_in = fopen(INPUT_FILE, "rb");
    if (!f_in) {
        fprintf(stderr, "Error: Cannot open input file: %s\n", INPUT_FILE);
        ((*encoder)->Uninitialize)(encoder);
        WelsDestroySVCEncoder(encoder);
        return -1;
    }
    
    FILE* f_out = fopen(OUTPUT_FILE, "wb");
    if (!f_out) {
        fprintf(stderr, "Error: Cannot open output file: %s\n", OUTPUT_FILE);
        fclose(f_in);
        ((*encoder)->Uninitialize)(encoder);
        WelsDestroySVCEncoder(encoder);
        return -1;
    }
    
    // 5. 准备输入/输出数据结构
    SFrameBSInfo info;
    memset(&info, 0, sizeof(SFrameBSInfo));

    SSourcePicture pic;
    memset(&pic, 0, sizeof(SSourcePicture));
    pic.iPicWidth = WIDTH;
    pic.iPicHeight = HEIGHT;
    pic.iColorFormat = videoFormatI420;
    
    // Stride (步幅) = 宽度
    pic.iStride[0] = pic.iPicWidth;
    pic.iStride[1] = pic.iPicWidth / 2;
    pic.iStride[2] = pic.iPicWidth / 2;
    
    // 分配内存用于读取一帧YUV
    unsigned char* yuv_buffer = (unsigned char*)malloc(FRAME_SIZE);
    if (!yuv_buffer) {
        fprintf(stderr, "Error: malloc failed for YUV buffer\n");
        fclose(f_in);
        fclose(f_out);
        ((*encoder)->Uninitialize)(encoder);
        WelsDestroySVCEncoder(encoder);
        return -1;
    }
    
    printf("Starting encoding %s -> %s\n", INPUT_FILE, OUTPUT_FILE);
    int frame_count = 0;

    // 6. 编码循环
    while (fread(yuv_buffer, 1, FRAME_SIZE, f_in) == FRAME_SIZE) {
        // 将YUV buffer的指针分配给 SSourcePicture
        // Y平面
        pic.pData[0] = yuv_buffer;
        // U平面
        pic.pData[1] = pic.pData[0] + Y_SIZE;
        // V平面
        pic.pData[2] = pic.pData[1] + UV_SIZE;

        // 7. 编码一帧
        rv = ((*encoder)->EncodeFrame)(encoder, &pic, &info);
        if (rv != 0) {
            fprintf(stderr, "EncodeFrame failed for frame %d. rv = %d\n", frame_count, rv);
            continue;
        }

        // 8. 将编码后的数据写入文件
        if (info.eFrameType != videoFrameTypeSkip) {
            for (int i = 0; i < info.iLayerNum; ++i) {
                SLayerBSInfo* layer = &info.sLayerInfo[i];
                
                // 1. 计算这一层的总字节数
                int total_layer_length = 0;
                for (int j = 0; j < layer->iNalCount; ++j) {
                    total_layer_length += layer->pNalLengthInByte[j];
                }

                // 2. 将所有 NAL 的数据一次性写入文件
                if (total_layer_length > 0) {
                    fwrite(layer->pBsBuf, 1, total_layer_length, f_out);
                }
            }
        }

        frame_count++;
        if (frame_count % 10 == 0) {
            printf("Encoded %d frames...\n", frame_count);
        }
    }

    // 9. 清理
    printf("Encoding finished. Total frames: %d\n", frame_count);
    
    free(yuv_buffer);
    fclose(f_in);
    fclose(f_out);
    ((*encoder)->Uninitialize)(encoder);
    WelsDestroySVCEncoder(encoder);

    return 0;
}