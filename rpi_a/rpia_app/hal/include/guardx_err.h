#ifndef GUARDX_ERR_H
#define GUARDX_ERR_H

/* GuardX_App_Convention.md: 모든 hal/registry 함수는 성공 시
 * GUARDX_OK(0), 실패 시 음수 에러코드 반환 */
typedef enum {
    GUARDX_OK            = 0,
    GUARDX_ERR_OPEN      = -1,   /* 디바이스 open 실패 */
    GUARDX_ERR_READ      = -2,   /* read 실패 */
    GUARDX_ERR_CLOSE     = -3,   /* close 실패 */
    GUARDX_ERR_NOT_OPEN  = -4,   /* open 안 된 상태에서 read/close */
    GUARDX_ERR_INVALID   = -5,   /* 잘못된 인자 */
} guardx_err_t;

#endif /* GUARDX_ERR_H */
