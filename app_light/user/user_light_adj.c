
#include "ets_sys.h"
#include "osapi.h"
#include "os_type.h"
#include "mem.h"
#include "user_interface.h"

#include "user_light.h"
#include "user_light_adj.h"
#include "pwm.h"
#include "user_light_hint.h"


#if LIGHT_DEVICE


#define ABS_MINUS(x,y) (x<y?(y-x):(x-y))
uint8 light_sleep_flg = 0;

uint16  min_ms = 15;
uint16  min_us = 15000;

bool change_finish=true;
os_timer_t timer_pwm_adj;

static u8 cur_ctrl_mode = 0; 
static u8 last_ctrl_mode = 0;       
/*
0 :  short time change.     now = (15*now + target)/16
1 :  10min, 1s case          
*/ 

uint32  target_duty[PWM_CHANNEL] = {0};

//For ctrl mode 1
typedef struct duty_step_s
{
    u32 integer;
    u32 fraction;
    u8 plus_minus;      // 0: plus;    1: minus
}duty_step_t;

static duty_step_t duty_step[PWM_CHANNEL] ;//step number to change the color
static u32 duty_step_fraction_now[PWM_CHANNEL] ={0} ;

//For ctrl mode 0
static u32 duty_now[PWM_CHANNEL] = {0};
static u32 duty_now_frac[PWM_CHANNEL] = {0};

int timer_cnt = 0;//a counter to record timer to shut down the light, 20*timer_cnt seconds in all
os_timer_t shut_down_t;
bool broadcast_shutdown = false;

//-----------------------------------Light para storage---------------------------
#define LIGHT_EVT_QNUM (40) 

static struct pwm_param LightEvtArr[LIGHT_EVT_QNUM];
static u8 CurFreeLightEvtIdx = 0;
static u8 TotalUsedLightEvtNum = 0;
static u8  CurEvtIdxToBeUse = 0;

static struct pwm_param *ICACHE_FLASH_ATTR LightEvtMalloc(void)
{
    struct pwm_param *tmp = NULL;
    TotalUsedLightEvtNum++;
    if(TotalUsedLightEvtNum > LIGHT_EVT_QNUM ){
        TotalUsedLightEvtNum--;
    }
    else{
        tmp = &(LightEvtArr[CurFreeLightEvtIdx]);
        CurFreeLightEvtIdx++;
        if( CurFreeLightEvtIdx > (LIGHT_EVT_QNUM-1) )
            CurFreeLightEvtIdx = 0;
    }
    return tmp;
}

static void ICACHE_FLASH_ATTR LightEvtFree(void)
{
    TotalUsedLightEvtNum--;
}

static void ICACHE_FLASH_ATTR light_pwm_smooth_adj_proc(void);


void ICACHE_FLASH_ATTR
light_save_target_duty()
{
    extern struct light_saved_param light_param;
    os_memcpy(light_param.pwm_duty,target_duty,sizeof(light_param.pwm_duty));
    light_param.pwm_period =  pwm_get_period();
    #if SAVE_LIGHT_PARAM
        spi_flash_erase_sector(PRIV_PARAM_START_SEC + PRIV_PARAM_SAVE);
        spi_flash_write((PRIV_PARAM_START_SEC + PRIV_PARAM_SAVE) * SPI_FLASH_SEC_SIZE,(uint32 *)&light_param, sizeof(struct light_saved_param));
    #endif
}


void ICACHE_FLASH_ATTR
light_set_aim_r(uint32 r)
{
    target_duty[LIGHT_RED]=r;
    light_pwm_smooth_adj_proc();
}

void ICACHE_FLASH_ATTR
light_set_aim_g(uint32 g)
{
    target_duty[LIGHT_GREEN]=g;
    light_pwm_smooth_adj_proc();
}

void ICACHE_FLASH_ATTR
light_set_aim_b(uint32 b)
{
    target_duty[LIGHT_BLUE]=b;
    light_pwm_smooth_adj_proc();
}

void ICACHE_FLASH_ATTR
light_set_aim_cw(uint32 cw)
{
    target_duty[LIGHT_COLD_WHITE]=cw;
    light_pwm_smooth_adj_proc();
}

void ICACHE_FLASH_ATTR
light_set_aim_ww(uint32 ww)
{
    target_duty[LIGHT_WARM_WHITE]=ww;
    light_pwm_smooth_adj_proc();
}

LOCAL bool ICACHE_FLASH_ATTR
check_pwm_current_duty_diff()
{
    int i;
    for(i=0;i<PWM_CHANNEL;i++){
        if(pwm_get_duty(i) != target_duty[i]){
            return true;
        }
    }
    return false;
}


void ICACHE_FLASH_ATTR light_dh_pwm_adj_proc(void *Targ)
{
    //step 1:  process the light course.
    uint8 i;
    for(i=0;i<PWM_CHANNEL;i++){	
        if( cur_ctrl_mode ==0 ){
            duty_now[i] = (duty_now[i]*15 + target_duty[i] + duty_now_frac[i])>>4;
            duty_now_frac[i] = (duty_now[i]*15 + target_duty[i] + duty_now_frac[i])%16;
        }
        else if (cur_ctrl_mode ==1){
            if(duty_step[i].plus_minus == 0)
                duty_now[i] +=duty_step[i].integer;
            else 
                duty_now[i] -=duty_step[i].integer;
            duty_step_fraction_now[i] += duty_step[i].fraction;
            if(duty_step_fraction_now[i]>=600) {
                if(duty_step[i].plus_minus == 0) duty_now[i]++;  
				else duty_now[i]--; 
                duty_step_fraction_now[i]-=600;
            }
        
        }
        user_light_set_duty(duty_now[i],i);
    }
    pwm_start();	
	
    //step 2: Check if the course is finished.
    if(check_pwm_current_duty_diff()){
        change_finish = 0;		
        os_timer_disarm(&timer_pwm_adj);
        os_timer_setfn(&timer_pwm_adj, (os_timer_func_t *)light_dh_pwm_adj_proc, NULL);
        os_timer_arm(&timer_pwm_adj, min_ms, 0);	
    }
    else{
        change_finish = 1;	
        os_timer_disarm(&timer_pwm_adj);
        light_pwm_smooth_adj_proc();
    }

}

bool ICACHE_FLASH_ATTR
check_pwm_duty_zero()
{
    int i;
    for(i=0;i<PWM_CHANNEL;i++){
        if(pwm_get_duty(i) != 0){
            return false;
        }
    }
    return true;
}


static void ICACHE_FLASH_ATTR light_pwm_smooth_adj_proc(void)
{
    if( TotalUsedLightEvtNum>0 ){
        //step1 if ctrl mode is changed, then stop last ctrl mode.
        if(last_ctrl_mode != cur_ctrl_mode){
            os_timer_disarm(&timer_pwm_adj);
            change_finish = 1;
        }
        //step2 ctrl val according to current ctrl mode.        
        if( cur_ctrl_mode ==0 ){
            user_light_set_period( LightEvtArr[CurEvtIdxToBeUse].period );
            os_memcpy(target_duty,LightEvtArr[CurEvtIdxToBeUse].duty,sizeof(target_duty));
            min_ms = 15;
        }else if (cur_ctrl_mode ==1){
            os_timer_disarm(&timer_pwm_adj);
            change_finish = 1;
            user_light_set_period( LightEvtArr[CurEvtIdxToBeUse].period );
            os_memcpy(target_duty,LightEvtArr[CurEvtIdxToBeUse].duty,sizeof(target_duty));
            min_ms = 1000;      // total 600 steps. finished
            uint8 i;
            for(i=0;i<PWM_CHANNEL;i++){
                duty_step_fraction_now[i] = 0;
                duty_step[i].integer = ABS_MINUS( target_duty[i] , duty_now[i] )/(600);
                duty_step[i].fraction = ABS_MINUS( target_duty[i] , duty_now[i] )%(600);
                duty_step[i].plus_minus = (target_duty[i] >= duty_now[i]) ? 0: 1;
            }
        }
        last_ctrl_mode = cur_ctrl_mode;
        
        //step3  common handle.      
        CurEvtIdxToBeUse++;
        if(CurEvtIdxToBeUse > (LIGHT_EVT_QNUM-1) ){
            CurEvtIdxToBeUse = 0;
        }
        LightEvtFree();
        if(change_finish){
            light_dh_pwm_adj_proc(NULL);
        }
    }
    
    if(change_finish){
        light_save_target_duty();
        if(check_pwm_duty_zero()){
            if(light_sleep_flg==0){
                os_printf("light sleep en\r\n");
                wifi_set_sleep_type(LIGHT_SLEEP_T);
                light_sleep_flg = 1;
            }
        }
    }
}



#if LIGHT_CURRENT_LIMIT
/*calculate the current of led, must accord with your own test result of your led*/
uint32 ICACHE_FLASH_ATTR
light_get_cur(uint32 duty , uint8 channel, uint32 period)
{
    uint32 duty_max_limit = (period*1000/45);
    uint32 duty_mapped = duty*22222/duty_max_limit;
    switch(channel){
        case LIGHT_RED : 
            if(duty_mapped>=0 && duty_mapped<23000){
                return (duty_mapped*151000/22222);
            }
            break;
        case LIGHT_GREEN:
            if(duty_mapped>=0 && duty_mapped<23000){
                return (duty_mapped*85000/22222);
            }
            break;
        case LIGHT_BLUE:
            if(duty_mapped>=0 && duty_mapped<23000){
                return (duty_mapped*75000/22222);
            }
            break;
        case LIGHT_COLD_WHITE:
        case LIGHT_WARM_WHITE:
            if(duty_mapped>=0 && duty_mapped<23000){
                return (duty_mapped*180000/22222);
            }
            break;
        default:
            os_printf("CHANNEL ERROR IN GET_CUR\r\n");
            break;
    }
}

#endif


/*set the color of light*/
void ICACHE_FLASH_ATTR
light_set_aim(uint32 r,uint32 g,uint32 b,uint32 cw,uint32 ww,uint32 period,u8 ctrl_mode)
{
    //os_printf("light_set_aim\r\n");
    struct pwm_param *tmp = LightEvtMalloc();    
    if(tmp != NULL){
        tmp->period = (period<10000?period:10000);
        uint32 duty_max_limit = (period*1000/45);
        
        tmp->duty[LIGHT_RED] = (r<duty_max_limit?r:duty_max_limit);
        tmp->duty[LIGHT_GREEN] = (g<duty_max_limit?g:duty_max_limit);
        tmp->duty[LIGHT_BLUE] = (b<duty_max_limit?b:duty_max_limit);
        tmp->duty[LIGHT_COLD_WHITE] = (cw<duty_max_limit?cw:duty_max_limit);
        tmp->duty[LIGHT_WARM_WHITE] = (ww<duty_max_limit?ww:duty_max_limit);//chg
        
        #if LIGHT_CURRENT_LIMIT
            uint32 cur_r,cur_g,cur_b,cur_rgb;
            
            //if(cw>0 || ww>0){
                cur_r = light_get_cur(tmp->duty[LIGHT_RED] , LIGHT_RED, tmp->period);
                cur_g =  light_get_cur(tmp->duty[LIGHT_GREEN] , LIGHT_GREEN, tmp->period);
                cur_b =  light_get_cur(tmp->duty[LIGHT_BLUE] , LIGHT_BLUE, tmp->period);
                cur_rgb = (cur_r+cur_g+cur_b);
            //}
            uint32 cur_cw = light_get_cur( tmp->duty[LIGHT_COLD_WHITE],LIGHT_COLD_WHITE, tmp->period);
            uint32 cur_ww = light_get_cur( tmp->duty[LIGHT_WARM_WHITE],LIGHT_WARM_WHITE, tmp->period);
            uint32 cur_remain,cur_mar;
            cur_remain = (LIGHT_TOTAL_CURRENT_MAX - cur_rgb -LIGHT_CURRENT_MARGIN);
            cur_mar = LIGHT_CURRENT_MARGIN;
    		
            while((cur_cw+cur_ww) > cur_remain){
                tmp->duty[LIGHT_COLD_WHITE] =  tmp->duty[LIGHT_COLD_WHITE] * 9 / 10;
                tmp->duty[LIGHT_WARM_WHITE] =  tmp->duty[LIGHT_WARM_WHITE] * 9 / 10;
                cur_cw = light_get_cur( tmp->duty[LIGHT_COLD_WHITE],LIGHT_COLD_WHITE, tmp->period);
                cur_ww = light_get_cur( tmp->duty[LIGHT_WARM_WHITE],LIGHT_WARM_WHITE, tmp->period);
            }	    
        #endif
        os_printf("prd:%u  r : %u  g: %u  b: %u  cw: %u  ww: %u \r\n",period,
        tmp->duty[0],tmp->duty[1],tmp->duty[2],tmp->duty[3],tmp->duty[4]);
        cur_ctrl_mode = ctrl_mode;
        light_pwm_smooth_adj_proc();
    }
    else{
        os_printf("light para full\n");
    }
}

void ICACHE_FLASH_ATTR light_adj_init(void)
{
    uint8 i;
    for(i=0;i<PWM_CHANNEL;i++){
        duty_now[i] = user_light_get_duty(i);
    }
}

void ICACHE_FLASH_ATTR
	light_TimerCheck(void* broadcast_if)
{
    timer_cnt -= 1;
    if(timer_cnt<=0){
		int bcst_if = (uint32)broadcast_if;
		#if 0
		if(bcst_if){
			light_SendMeshBroadcastCmd(0,0,0,0,0,1000);
		}
		#endif
        light_set_aim(0,0,0,0,0,1000,0);
        timer_cnt = 0;
        os_timer_disarm(&shut_down_t);
    }
}

/*push the button once, add another 20 seconds*/
void ICACHE_FLASH_ATTR
light_TimerAdd(uint32* duty,uint32 t,bool add_if,uint32 broadcast_if)
{
    if(timer_cnt==0){
		
        if(duty){
            light_set_aim(duty[0],duty[1],duty[2],duty[3],duty[4],1000,0);
        }else{
            if(check_pwm_duty_zero()){
                light_set_aim(0,0,0,22222,22222,1000,0);
            }else{
                //light_shadeStart(HINT_WHITE,500,1,1,NULL);
            }
        }
		if(add_if){
            os_timer_disarm(&shut_down_t);
            os_timer_setfn(&shut_down_t,light_TimerCheck,NULL);
            os_timer_arm(&shut_down_t,t,1);
		}
    }
	
    light_shadeStart(HINT_WHITE,500,1,1,NULL);
	
	if(add_if){
		timer_cnt++;
	}else{
	    timer_cnt = 1;
		os_timer_disarm(&shut_down_t);
		os_timer_setfn(&shut_down_t,light_TimerCheck,(void*)broadcast_if);
		os_timer_arm(&shut_down_t,t,0);
	}
	
}

void ICACHE_FLASH_ATTR
light_TimerStop()
{
    os_timer_disarm(&shut_down_t);
    timer_cnt = 0;
}
#endif
