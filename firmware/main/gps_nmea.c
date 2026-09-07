#include "gps_nmea.h"
#include <string.h>

/* 内部行缓冲：feed_line 把句子拷进来就地切分。非重入，理由见头文件。 */
static char s_line[GPS_NMEA_LINE_MAX];

static int hexval(char c){
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* split in-place by ',' → fields[]; returns field count（自 gps_task.c 逐字迁入） */
static int split_csv(char *s, char *fields[], int maxf){
    int n = 0; char *p = s;
    fields[n++] = p;
    while(*p && n < maxf){ if(*p == ','){ *p = '\0'; fields[n++] = p + 1; } p++; }
    return n;
}

int gps_nmea_feed_line(gps_nmea_msg_t *out, const char *line){
    if(!out || !line) return 0;
    out->n = 0;
    out->type[0] = '\0';

    if(line[0] != '$') return 0;
    if(strlen(line) >= GPS_NMEA_LINE_MAX) return 0;   /* 超长整句拒收，不截半句 */

    /* checksum 门 —— 这次抽取的要点：老 handle_line 不验 *hh，损坏句子的
     * 字段会直接进 fix/校时。校验放在切分之前，坏句子不碰缓冲。 */
    const char *star = strchr(line, '*');
    if(!star) return 0;
    int cs = 0;
    for(const char *p = line + 1; p < star; p++) cs ^= (unsigned char)*p;
    /* 短路顺序保证 star[2] 只在 star[1] 是合法 hex 时才被读。 */
    if(hexval(star[1]) < 0 || hexval(star[2]) < 0 || star[3] != '\0') return 0;
    if(cs != ((hexval(star[1]) << 4) | hexval(star[2]))) return 0;

    /* 拷入内部缓冲并截掉 checksum 段（'$' 与 '*' 都不含，f[0] 才是 addr）
     * → 切出干净 CSV。 */
    size_t head = (size_t)(star - line) - 1;
    memcpy(s_line, line + 1, head);
    s_line[head] = '\0';

    char *fields[GPS_NMEA_MAX_FIELDS];
    int n = split_csv(s_line, fields, GPS_NMEA_MAX_FIELDS);

    /* type = addr 末 3 字符（老后缀语义）；addr 不足 3 字符视为非法整句。 */
    size_t tl = strlen(fields[0]);
    if(tl < 3) return 0;
    memcpy(out->type, fields[0] + (tl - 3), 3);
    out->type[3] = '\0';

    for(int i = 0; i < n; i++) out->f[i] = fields[i];
    out->n = n;
    return 1;
}
