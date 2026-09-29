#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "micro_mp3/mp3_decoder.h"
#include "audio.h"

#define MP3_TAG "DEC_MP3"
#define MP3_INPUT_CHUNK_SIZE 2048        /* 每次读入的压缩数据块大小 */
#define MP3_PCM_BUF_SAMPLES  (1152 * 2)  /* 一帧最大 PCM 样本数 */

#define ID3_FRAME_TITLE   "TIT2"   /* ID3v2 标题帧 ID */
#define ID3_FRAME_ARTIST  "TPE1"   /* 艺术家帧 ID */
#define ID3_FRAME_ALBUM   "TALB"   /* 专辑帧 ID */
#define ID3_FRAME_COVER   "APIC"   /* 图片(封面)帧 ID */

#define COVER_MAX_SIZE    (512 * 1024)   /* 封面大小上限, 防止超大图吃光 PSRAM */

/* MP3 解码器实例 */
typedef struct {
    audio_decoder_t iface;

    audio_stream_t        *in;         /* 输入流 (音频任务提供的预读流) */
    micro_mp3::Mp3Decoder *mp3;        /* micro-mp3 解码器对象 */
    uint8_t               *inbuf;      /* 压缩输入缓冲 */
    size_t                 in_off;     /* 缓冲内当前消费偏移 */
    size_t                 in_len;     /* 缓冲内剩余有效字节数 */
    bool                   eof;        /* 已到文件尾 */
    bool                   info_done;  /* 流信息是否已解析 */
    int16_t                pcm_buf[MP3_PCM_BUF_SAMPLES];   /* 解码 PCM 临时缓冲 */

    uint32_t sample_rate;   /* 采样率 (首帧解析后获得) */
    uint8_t  channels;      /* 声道数 */

    char     title[SONG_TITLE_MAX];      /* 标题 (ID3 TIT2) */
    char     artist[SONG_ARTIST_MAX];    /* 歌手 (ID3 TPE1) */
    uint32_t file_size;                  /* 文件大小 */

    uint8_t *cover_data;   /* APIC 图片字节 (PSRAM), 未移交时由 mp3_close 释放 */
    size_t   cover_size;   /* 封面大小 */
} decoder_mp3_t;

/* 读 ID3v2 syncsafe 32bit (每字节只用 7 位) */
static uint32_t syncsafe(const uint8_t *p)
{
    return ((uint32_t)p[0] << 21) | ((uint32_t)p[1] << 14)
         | ((uint32_t)p[2] <<  7) |  (uint32_t)p[3];
}

/* 读大端 32bit */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/* 解析 ID3v2 标签: 提取标题/歌手/封面.
 * f=文件, off=输出 MP3 数据起始偏移, title_out/artist_out=输出缓冲,
 * cover_out=封面指针(PSRAM), cover_size_out=封面大小. */
static void parse_id3v2(audio_stream_t *in, long *off,
                        char *title_out, size_t title_size,
                        char *artist_out, size_t artist_size,
                        uint8_t **cover_out, size_t *cover_size_out)
{
    *off = 0;
    uint8_t h[10];
    if(in->read(in,h,10)!=10 || memcmp(h,"ID3",3)) { in->seek(in,0,SEEK_SET); return; }   /* 非 ID3 头 */
    /* ID3v2 头: [0..2]="ID3", [3]=主版本, [4]=修订号, [5]=标志, [6..9]=标签大小(同步安全整数).
     * 标志位: 0x80=非同步, 0x40=扩展头, 0x20=实验, 0x10=footer(仅 v2.4).
     * 注意标志在 h[5] 而非 h[4] (h[4] 是修订号). */
    uint8_t v=h[3]; bool ft=(h[5]&0x10); uint32_t ts=syncsafe(h+6);   /* 版本/footer标志/标签大小 */
    ESP_LOGI(MP3_TAG, "ID3v2.%u 标签, 大小 %" PRIu32, v, ts);
    uint32_t pos=10;   /* 当前标签内偏移 */
    while(pos<ts){
        uint8_t fh[10]; if(in->read(in,fh,10)!=10) break; pos+=10;   /* 帧头: ID + 大小 + 标志 */
        if(fh[0]==0){in->seek(in,-10,SEEK_CUR);break;}   /* 帧 ID 为空 = 标签结束 */
        uint32_t fs=(v>=4)?syncsafe(fh+4):be32(fh+4);   /* v2.4 用 syncsafe, 更早用普通大端 */
        if(fs>ts-pos)break;
        uint8_t *d=(uint8_t*)malloc(fs);
        if(!d||in->read(in,d,fs)!=fs){free(d);break;} pos+=fs;   /* 读帧数据 */
        if(!memcmp(fh,ID3_FRAME_TITLE,4)&&fs>1){   /* 标题 (跳过首字节编码标志) */
            size_t n=(size_t)(fs-1); if(n>=title_size)n=title_size-1;
            if(title_out){memcpy(title_out,d+1,n);title_out[n]='\0';}
            printf("[ID3] 标题: %.*s\n",(int)(fs-1),d+1);
        }
        if(!memcmp(fh,ID3_FRAME_ARTIST,4)&&fs>1){   /* 歌手 */
            size_t n=(size_t)(fs-1); if(n>=artist_size)n=artist_size-1;
            if(artist_out){memcpy(artist_out,d+1,n);artist_out[n]='\0';}
            printf("[ID3] 艺术家: %.*s\n",(int)(fs-1),d+1);
        }
        if(!memcmp(fh,ID3_FRAME_ALBUM,4)&&fs>1)  printf("[ID3] 专辑: %.*s\n",(int)(fs-1),d+1);
        if(!memcmp(fh,ID3_FRAME_COVER,4)&&fs>4){   /* 封面 APIC: MIME\0类型\0描述\0图片 */
            const char*m=(const char*)(d+1); size_t ml=strlen(m);   /* MIME 类型串 */
            const uint8_t*p=d+1+ml+1; uint8_t pt=*p++;   /* 图片类型字节 */
            while(*p&&p<d+fs)p++;   /* 跳过描述串 */
            p++;
            size_t isz=d+fs-p;   /* 剩余 = 图片数据 */
            printf("[ID3] 封面: MIME=%s, 类型=%s, 大小=%zu bytes\n", m,
                   pt==3?"封面(正面)":pt==4?"封面(背面)":"其他", isz);
            /* 只取第一张封面, 拷贝到 PSRAM */
            if (cover_out && *cover_out == NULL && isz > 0 && isz <= COVER_MAX_SIZE) {
                uint8_t *buf = (uint8_t*)heap_caps_malloc(isz, MALLOC_CAP_SPIRAM);
                if (buf) {
                    memcpy(buf, p, isz);
                    *cover_out = buf;
                    if (cover_size_out) *cover_size_out = isz;
                    printf("[ID3] 封面字节已提取到 PSRAM: %zu bytes\n", isz);
                }
            }
        }
        free(d);
    }
    *off=10+ts+(ft?10:0);   /* 音频偏移 = 10 字节头 + 标签体 (+ 10 字节 footer, 仅 v2.4; 扩展头已计入 ts) */
}

/* 打开 MP3 文件: 解析 ID3, 创建解码器 */
static bool mp3_open(audio_decoder_t *iface, const char *path, audio_stream_t *in)
{
    decoder_mp3_t *d = (decoder_mp3_t *)iface;
    (void)path;

    /* 释放上次残留封面 (防止重复 open 泄漏) */
    if (d->cover_data) {
        heap_caps_free(d->cover_data);
        d->cover_data = NULL;
        d->cover_size = 0;
    }

    d->in = in;

    long off = 0;
    parse_id3v2(in, &off, d->title, sizeof(d->title), d->artist, sizeof(d->artist),
                &d->cover_data, &d->cover_size);   /* 先解析头部标签 */

    d->mp3 = new micro_mp3::Mp3Decoder();
    d->inbuf = (uint8_t *)heap_caps_malloc(MP3_INPUT_CHUNK_SIZE, MALLOC_CAP_DEFAULT);
    if(!d->mp3 || !d->inbuf){
        if(d->mp3){delete d->mp3; d->mp3=NULL;}
        if(d->inbuf){heap_caps_free(d->inbuf); d->inbuf=NULL;}
        if(d->cover_data){heap_caps_free(d->cover_data); d->cover_data=NULL; d->cover_size=0;}
        d->in = NULL;
        return false;
    }

    in->seek(in, off, SEEK_SET);   /* 定位到 MP3 音频数据起始 */
    d->eof  = false;
    d->info_done = false;
    d->in_off = 0;
    d->in_len = 0;

    {   /* 记录文件大小 */
        d->file_size = in->size(in);
        printf("[音频] 文件大小=%u bytes, ID3偏移=%ld → MP3数据=%u bytes\n",
               (unsigned)d->file_size, off, (unsigned)(d->file_size - (uint32_t)off));
    }

    return true;
}

/* 解码一帧: 喂压缩数据给解码器直到产出 PCM.
 * pcm=输出缓冲, bytes 输入容量/输出实际字节数 */
static bool mp3_decode_frame(audio_decoder_t *iface, int16_t *pcm, size_t *bytes)
{
    decoder_mp3_t *d = (decoder_mp3_t *)iface;

    if(!d->in || !d->mp3 || !d->inbuf) return false;

    while(1){
        if(d->in_len == 0){   /* 缓冲耗尽 → 重新读入一块 */
            d->in_len = d->in->read(d->in, d->inbuf, MP3_INPUT_CHUNK_SIZE);
            if(d->in_len == 0){ d->eof = true; return false; }
            d->in_off = 0;
        }

        const uint8_t *p = d->inbuf + d->in_off;
        size_t n = d->in_len, consumed = 0, samples = 0;
        auto res = d->mp3->decode(p, n, (uint8_t*)d->pcm_buf,
                                   sizeof(d->pcm_buf), consumed, samples);

        d->in_off += consumed;   /* 前进消费偏移 */
        d->in_len -= consumed;
        if(d->in_len == 0) d->in_off = 0;

        if(res == micro_mp3::MP3_STREAM_INFO_READY ||
           res == micro_mp3::MP3_STREAM_INFO_CHANGED){   /* 首帧解析到流信息 */
            if(!d->info_done){
                d->sample_rate = d->mp3->get_sample_rate();
                d->channels    = d->mp3->get_channels();
                printf("[音频] MP3: %" PRIu32 " Hz, %u ch, %" PRIu32 " kbps\n",
                       d->sample_rate, d->channels, d->mp3->get_bitrate());
                d->info_done = true;
            }
            continue;
        }
        if(res == micro_mp3::MP3_NEED_MORE_DATA){   /* 数据不足 */
            if(d->in_len == 0) continue;
            if(consumed == 0){
                if(d->in_off > 0){   /* 把剩余数据移到缓冲头部 */
                    memmove(d->inbuf, d->inbuf + d->in_off, d->in_len);
                    d->in_off = 0;
                }
                size_t nread = d->in->read(d->in, d->inbuf + d->in_len,   /* 追加读入 */
                                     MP3_INPUT_CHUNK_SIZE - d->in_len);
                if(nread == 0){ d->eof = true; return false; }
                d->in_len += nread;
            }
            continue;
        }
        if(res == micro_mp3::MP3_DECODE_ERROR) continue;   /* 容忍坏帧, 尝试下一帧 */
        if(res == micro_mp3::MP3_OUTPUT_BUFFER_TOO_SMALL) continue;
        if(res < 0){ d->eof = true; return false; }   /* 致命错误 */

        if(samples > 0){   /* 产出 PCM */
            size_t total = samples * d->mp3->get_channels() * d->mp3->get_bytes_per_sample();
            memcpy(pcm, d->pcm_buf, total);
            *bytes = total;
            return true;
        }
    }
}

static bool mp3_is_eof(audio_decoder_t *iface)
{
    return ((decoder_mp3_t *)iface)->eof;
}

static void mp3_close(audio_decoder_t *iface)
{
    decoder_mp3_t *d = (decoder_mp3_t *)iface;
    if(d->mp3){ delete d->mp3; d->mp3 = NULL; }
    if(d->inbuf){ heap_caps_free(d->inbuf); d->inbuf = NULL; }
    if(d->cover_data){ heap_caps_free(d->cover_data); d->cover_data = NULL; d->cover_size = 0; }
    d->in     = NULL;
    d->in_off = 0;
    d->in_len = 0;
    d->eof    = false;
}

static uint32_t mp3_get_sample_rate(audio_decoder_t *iface)
{
    return ((decoder_mp3_t *)iface)->sample_rate;
}

static uint8_t mp3_get_channels(audio_decoder_t *iface)
{
    return ((decoder_mp3_t *)iface)->channels;
}

/* MP3 位深固定 16bit */
static uint8_t mp3_get_bits(audio_decoder_t *iface)
{
    return 16;
}

static uint32_t mp3_get_bitrate(audio_decoder_t *iface)
{
    decoder_mp3_t *d = (decoder_mp3_t *)iface;
    return d->mp3 ? d->mp3->get_bitrate() : 0;
}

static uint32_t mp3_get_file_size(audio_decoder_t *iface)
{
    return ((decoder_mp3_t *)iface)->file_size;
}

/* 当前位置 = 文件偏移 (含 ID3, 时长估算会有偏差, 可接受) */
static uint32_t mp3_get_position(audio_decoder_t *iface)
{
    decoder_mp3_t *d = (decoder_mp3_t *)iface;
    if (!d->in) return 0;
    long pos = d->in->tell(d->in);
    return pos > 0 ? (uint32_t)pos : 0;
}

/* 按字节偏移 seek: 重置解码器, 丢弃输入缓冲重新定位 */
static bool mp3_seek(audio_decoder_t *iface, uint32_t byte_offset)
{
    decoder_mp3_t *d = (decoder_mp3_t *)iface;
    if(!d->in || !d->mp3) return false;
    if(byte_offset > d->file_size) byte_offset = d->file_size;
    if(!d->in->seek(d->in, (long)byte_offset, SEEK_SET)) return false;
    d->mp3->reset();   /* 解码器状态复位 */
    d->in_off = 0;
    d->in_len = 0;
    d->eof    = false;
    d->info_done = false;   /* 跳转后需重新解析流信息 */
    printf("[音频] SEEK -> %" PRIu32 " bytes\n", byte_offset);
    return true;
}

static const char *mp3_get_title(audio_decoder_t *iface)
{
    return ((decoder_mp3_t *)iface)->title;
}

static const char *mp3_get_artist(audio_decoder_t *iface)
{
    return ((decoder_mp3_t *)iface)->artist;
}

static const uint8_t *mp3_get_cover_data(audio_decoder_t *iface)
{
    return ((decoder_mp3_t *)iface)->cover_data;
}

static size_t mp3_get_cover_size(audio_decoder_t *iface)
{
    return ((decoder_mp3_t *)iface)->cover_size;
}

/* 所有权移交给封面模块后, 标记解码器不再持有, 避免 close 时重复释放 */
static void mp3_take_cover(audio_decoder_t *iface)
{
    decoder_mp3_t *d = (decoder_mp3_t *)iface;
    d->cover_data = NULL;
    d->cover_size = 0;
}

/* 工厂: 创建 MP3 解码器, 填充接口函数指针 */
audio_decoder_t *decoder_mp3_create(void)
{
    decoder_mp3_t *d = (decoder_mp3_t *)calloc(1, sizeof(decoder_mp3_t));
    if(!d) return NULL;

    d->iface.open            = mp3_open;
    d->iface.decode          = mp3_decode_frame;
    d->iface.is_eof          = mp3_is_eof;
    d->iface.close           = mp3_close;
    d->iface.get_sample_rate = mp3_get_sample_rate;
    d->iface.get_channels    = mp3_get_channels;
    d->iface.get_bits        = mp3_get_bits;
    d->iface.get_bitrate     = mp3_get_bitrate;
    d->iface.get_file_size   = mp3_get_file_size;
    d->iface.get_position    = mp3_get_position;
    d->iface.seek            = mp3_seek;
    d->iface.get_title       = mp3_get_title;
    d->iface.get_artist      = mp3_get_artist;
    d->iface.get_cover_data  = mp3_get_cover_data;
    d->iface.get_cover_size  = mp3_get_cover_size;
    d->iface.take_cover      = mp3_take_cover;

    return &d->iface;
}
