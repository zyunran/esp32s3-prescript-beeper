/* WEB 组件: 内嵌配置页 + REST API
 * 联网后手机/PC 浏览器访问 http://<esp32-ip>/ 打开配置页, 可:
 *   - 增删改指令库(每行一条, 支持 {#RRGGBB}/{} {RAND:1-10} {TIMER})
 *   - 设置闹钟(每天重复/按星期/一次性, 最多 3 个) / 待办管理
 *   - 改 UI 主题色(背景/菜单/选中框/图标/时钟/日期) / 音量/蜂鸣/熄屏时长
 *   - 改 WiFi/城市/天气私钥, 扫描附近 WiFi; 设使用者名称
 *   - 设备状态(电量/堆/开机次数/IP/时间/天气), 下发指令(含 made in heaven 彩蛋), 拼点人格图鉴
 * 全部写入 NVS 持久化(命名空间: 指令"ins" / 闹钟"alarm" / 颜色"cfg" / 设置"set" 等), 重启后仍生效.
 * API:
 *   GET  /api/cfg      -> 当前配置 JSON
 *   POST /api/cfg      -> 接收 JSON 应用并保存 {"ok":1}
 *   POST /api/beep|reboot|scan|send     -> 蜂鸣测试/重启/WiFi扫描/下发指令
 *   GET  /api/status|gacha|todo  POST /api/todo -> 设备状态/拼点图鉴/待办增删查
 */
#include "web.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "LCD.h"
#include "UI.h"
#include "INSTRUCTION.h"
#include "ALARM.h"
#include "NET.h"
#include "SETTING.h"
#include "GACHA.h"
#include "TODO.h"
#include "ANSWER.h"
#include "BATTERY.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "WEB";

/* 凭据脱敏掩码: /api/cfg 不回传 WiFi 密码/天气 key 明文(该端口 LAN 内任意设备可访问);
 * 配置页以掩码占位, 保存时遇到该串视为"保持不变", 不覆盖已存凭据 */
#define WEB_SECRET_MASK "********"

/* 配置已改标志: 网页保存配置/待办后置 1, ui_task 检测后重绘主界面应用(实时生效+绘制统一避免并发) */
static uint8_t web_dirty = 0;
uint8_t WEB_ConfigDirty(void)      { return web_dirty; }
void    WEB_ConfigDirtyClear(void) { web_dirty = 0; }

/* ================= 主题色项(名称<->全局变量) ================= */
typedef struct { const char *name; uint16_t *color; } web_color_t;
static const web_color_t web_colors[] = {
    { "bg",    &UI_COLOR_BG },
    { "menu",  &UI_COLOR_MENU },
    { "frame", &UI_COLOR_FRAME },
    { "icon",  &UI_COLOR_ICON },
    { "time",  &UI_COLOR_TIME },
    { "date",  &UI_COLOR_DATE },
};
#define WEB_COLOR_N (sizeof(web_colors) / sizeof(web_colors[0]))

/* 从 NVS 读主题色(缺省保持默认) */
static void web_colors_load(void)
{
    nvs_handle_t h;
    uint8_t i;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK)
    {
        for (i = 0; i < WEB_COLOR_N; i++)
        {
            uint16_t v;
            if (nvs_get_u16(h, web_colors[i].name, &v) == ESP_OK)
            {
                *web_colors[i].color = v;
            }
        }
        nvs_close(h);
    }
}

static void web_colors_save(void)
{
    nvs_handle_t h;
    uint8_t i;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK)
    {
        for (i = 0; i < WEB_COLOR_N; i++)
        {
            nvs_set_u16(h, web_colors[i].name, *web_colors[i].color);
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

/* RGB565 -> 6位RGB888十六进制(无#) */
static void rgb565_to_hex(uint16_t c, char *out)
{
    uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    snprintf(out, 8, "%02X%02X%02X",
             (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
}

/* 6位RGB888十六进制 -> RGB565 */
static uint16_t hex_to_rgb565(const char *hex)
{
    long v = strtol(hex, NULL, 16);
    uint8_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* UTF-8 合法性 + 最大字节数校验(WEB 输入防截断乱码) */
static int web_utf8_valid(const char *s, size_t max_len)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t left;
    if (!s) return 0;
    left = strlen(s);
    if (left > max_len) return 0;
    while (*p)
    {
        size_t need, i;
        if (*p < 0x80) { p++; left--; continue; }
        if ((*p & 0xE0) == 0xC0) { if (*p < 0xC2) return 0; need = 2; }
        else if ((*p & 0xF0) == 0xE0) need = 3;
        else if ((*p & 0xF8) == 0xF0) need = 4;
        else return 0;
        if (left < need) return 0;
        for (i = 1; i < need; i++)
        {
            if ((p[i] & 0xC0) != 0x80) return 0;
        }
        if (need == 3)
        {
            if (*p == 0xE0 && p[1] < 0xA0) return 0;
            if (*p == 0xED && p[1] > 0x9F) return 0;
        }
        else if (need == 4)
        {
            if (*p == 0xF0 && p[1] < 0x90) return 0;
            if (*p == 0xF4 && p[1] > 0x8F) return 0;
        }
        p += need;
        left -= need;
    }
    return 1;
}

/* 6位十六进制颜色(不含 #) */
static int web_hex_color_valid(const char *s)
{
    size_t i;
    if (!s || strlen(s) != 6) return 0;
    for (i = 0; i < 6; i++)
    {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

/* 指令库文本: 总长/NVS上限/UTF-8/每行长度/条数校验 */
static int web_ins_text_valid(const char *s)
{
    const char *p = s, *nl;
    uint8_t n = 0;
    if (!web_utf8_valid(s, 8192)) return 0;
    while (*p)
    {
        size_t len;
        nl = strchr(p, '\n');
        len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= INS_PRESET_LEN) return 0;
        if (len > 0)
        {
            n++;
            if (n > INS_PRESET_MAX) return 0;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 1;
}

/* ================= 配置页(内嵌 HTML, 面向小白: 状态栏/分组/快捷配色) ================= */
static const char web_page[] =
"<!DOCTYPE html><html lang=zh><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>BB机设置</title><style>"
"body{font-family:system-ui;max-width:560px;margin:16px auto;padding:0 12px;background:#141a14;color:#c8e0c0}"
"h1{font-size:18px;margin:0 0 4px}.sub{font-size:12px;color:#8a9a8a;margin-bottom:12px}"
".st{font-size:13px;padding:8px;border-radius:6px;margin-bottom:12px}.st.ok{background:#123a1a;color:#90e0a0}.st.no{background:#3a2412;color:#ffb070}"
"h2{font-size:14px;color:#8fd48a;border-bottom:1px solid #2a4a2a;padding-bottom:4px;margin:18px 0 8px}"
"label{display:block;margin:8px 0 2px;font-size:13px;color:#a8c8a0}"
"input[type=text],input[type=password],input[type=number],select{width:200px;background:#0e120e;color:#e8f0e8;border:1px solid #3a5a3a;padding:5px}"
"textarea{width:100%;height:200px;box-sizing:border-box;background:#0e120e;color:#e8f0e8;border:1px solid #3a5a3a}"
".row{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin:4px 0}.row label{margin:0}"
"input[type=color]{width:52px;height:28px;border:0;background:none;vertical-align:middle}"
".hint{font-size:12px;color:#6a8a6a}.alarm{display:flex;gap:8px;align-items:center;margin:4px 0;flex-wrap:wrap}.alarm input[type=number]{width:60px}.alarm input[type=checkbox]{width:auto}"
".btns{margin:16px 0 24px;display:flex;gap:10px;flex-wrap:wrap}button{padding:10px 20px;font-size:15px;border:0;border-radius:6px;cursor:pointer}"
"#save{background:#2a6a2a;color:#e8ffe8}#beep{background:#3a3a5a;color:#e0e0ff}#reboot{background:#5a2a2a;color:#ffe0e0}"
"#msg{margin:6px 0;font-size:14px;color:#90e090;min-height:18px}</style></head><body>"
"<h1>📟 BB 机设置</h1><div class=sub>给传呼机设置 WiFi、指令、外观、闹钟。改完点「保存到设备」。</div>"
"<div class='st' id=st>加载中…</div>"
"<h2>① 连上家里的 WiFi</h2><div class=hint>点「扫描」选你家 WiFi 填密码; 手机也要连同一个 WiFi 才能继续用网页。</div>"
"<button id=scanwifi type=button style='background:#1f5f9f;color:#fff;padding:8px 16px;border:0;border-radius:6px;margin-bottom:8px'>扫描附近 WiFi</button>"
"<div id=wifilist style='max-height:180px;overflow:auto;border:1px solid #3a5a3a;border-radius:4px;margin-bottom:8px;display:none'></div>"
"<label>WiFi 名</label><input id=ssid type=text placeholder='扫描选择或手输'>"
"<label>WiFi 密码(已配置不回显, 星号=保持不变)</label><input id=pass type=password>"
"<div class=row><button id=clrwifi type=button style='background:#5a2a2a;color:#fff;padding:6px 14px;border:0;border-radius:6px' onclick=clrwifi()>清除已存WiFi(回配网模式)</button></div>"
"<label>天气城市(拼音)</label><input id=city type=text placeholder='chengdu / beijing'>"
"<label>天气 API 私钥(心知天气)</label><input id=key type=text placeholder='留空用内置默认'>"
"<h2>② 指令库(神谕随机抽取)</h2><div class=hint>每行一条; 支持 {#FF0000}颜色 / {RAND:1-10}随机数 / {TODO}待办</div><textarea id=ins></textarea>"
"<h2>⑨ 答案之书(询问)</h2><div class=hint>文本框显示并编辑该分类【全部】答案(内置+自定义); 每行一条, 保存到设备生效; 清空保存=恢复内置默认</div>"
"<div class=row><b style=color:#8fd48a>回答</b></div><textarea id=ans0 rows=4 style='width:100%;box-sizing:border-box'></textarea>"
"<div class=row><b style=color:#8fd48a>吃什么</b></div><textarea id=ans1 rows=3 style='width:100%;box-sizing:border-box'></textarea>"
"<div class=row><b style=color:#8fd48a>喝什么</b></div><textarea id=ans2 rows=3 style='width:100%;box-sizing:border-box'></textarea>"
"<div class=row><b style=color:#8fd48a>玩什么</b></div><textarea id=ans3 rows=3 style='width:100%;box-sizing:border-box'></textarea>"
"<label>当前使用者(无「致X:」的指令自动加致TA)</label><div class=row><select id=user></select>"
"<input id=useradd type=text placeholder='新名字' style='width:80px'><button onclick=userAdd() style='background:#2a5a8f;color:#fff;padding:4px 10px;border:0;border-radius:4px'>添加</button></div>"
"<h2>③ 外观(主题色)</h2>"
"<div class=row><label>快捷配色</label><select id=preset onchange=preset(this.value)><option value=''>自定义</option></select></div>"
"<div class=row id=cols></div>"
"<h2>破译外观</h2><div class=hint>乱码破译的真字色/乱码色/速度/字号(行数自动匹配)</div>"
"<div class=row><label>真字 <input type=color id=gdef></label><label>乱码 <input type=color id=ggb></label></div>"
"<div class=row><label>乱码刷新 <input id=gdl type=number min=5 max=100 style='width:64px'>ms</label><label>逐字揭示 <input id=grv type=number min=10 max=300 style='width:64px'>ms</label>"
"<label>字号 <select id=gfnt><option value=0>16px</option><option value=1>24px</option><option value=2>32px</option></select></label></div>"
"<h2>④ 闹钟</h2><div class=hint>最多16个; 点下方「+ 添加闹钟」增加, 每行可删; 模式=每天/工作日/周末/一次性/自定义, 到点屏幕显示「今日指令」</div><div id=alarms></div>"
"<h2>⑤ 声音与系统</h2>"
"<div class=row><label>蜂鸣器 <input id=beep type=checkbox style='width:auto'></label><label>音量 <input id=vol type=number min=0 max=100></label></div>"
"<div class=row><label>熄屏 <select id=timeout><option value=30>30秒</option><option value=60>1分</option><option value=300>5分</option><option value=0>永不</option></select></label>"
"<label>神谕 <select id=on><option value=0>关</option><option value=1>1条</option><option value=3>3条</option><option value=5>5条</option><option value=9>9条</option></select></label>"
"<label>时段 <select id=ow><option value=0>白天</option><option value=1>全天</option><option value=2>晚上</option><option value=3>凌晨</option></select></label></div>"
"<div class=row><label>光标 <select id=cursor><option value=0>白线</option><option value=1>白块</option><option value=2>角框</option></select></label></div>"
"<div class=btns><button id=save onclick=save()>保存到设备</button><button id=beep onclick=beep()>试响</button><button id=reboot onclick=reboot()>重启设备</button></div>"
"<div id=msg></div>"
"<h2>⑥ 抽卡图鉴</h2><div class=hint>已抽中的人格才可拼点。★=已抽, ▢=还没抽到</div>"
"<div class=row>共 <b id=gtotal>0</b> 个 · 已抽 <b id=gowned>0</b> 个</div>"
"<div id=gacha style='font-size:12px;max-height:260px;overflow:auto;border:1px solid #3a5a3a;border-radius:4px;padding:6px;margin-top:6px'></div>"
"<h2>⑦ 待办(指令日志)</h2><div class=hint>要执行的事; 指令前加 {TODO} 会在设备破译时自动存入</div>"
"<div class=row><input id=txt type=text placeholder='要执行的事' style='flex:1'><button onclick=todoAdd() style='background:#2a5a8f;color:#fff;padding:8px 16px;border:0;border-radius:6px'>添加</button><button onclick=todoClear() style='background:#5a2a2a;color:#fff;padding:8px 16px;border:0;border-radius:6px'>清空</button></div>"
"<div id=todolist></div>"
"<h2>⑧ 设备状态</h2><div id=devst class=hint>加载中…</div><button onclick=devst() style='background:#2a5a8f;color:#fff;padding:8px 16px;border:0;border-radius:6px;margin-top:6px'>刷新</button>"
"<h2>📨 发指令给 BB 机</h2><div class=hint>输入文字点发送, 设备立刻乱码破译显示</div>"
"<div class=row><input id=sendtxt type=text placeholder='如: 去喝水' maxlength='96' style='flex:1'><button onclick=sendcmd() style='background:#2a5a8f;color:#fff;padding:8px 16px;border:0;border-radius:6px'>发送</button></div>"
"<div id=msg2></div>"
"<script>"
"const COLS=['bg','menu','frame','icon','time','date'],CN=['背景','菜单','选中框','图标','时钟','日期'];\n"
"function esc(s){return String(s).replace(/[&<>\"']/g,function(c){return '&#'+c.charCodeAt(0)+';';});}"
"const P={'护眼绿':{bg:'141A14',menu:'C8E0C0',frame:'8FD48A',icon:'7FD0D0',time:'C8E0C0',date:'C8E0C0'},'深空黑':{bg:'101018',menu:'C0C0D8',frame:'7080F0',icon:'40A0E0',time:'C0C0D8',date:'808090'},'暖橙':{bg:'181410',menu:'E0D0C0',frame:'FF9040',icon:'FFB070',time:'E0D0C0',date:'A08060'},'星云紫':{bg:'141018',menu:'D8C8E8',frame:'A070F0',icon:'80C0F0',time:'D8C8E8',date:'807090'},'赛博青':{bg:'081018',menu:'B8E8F0',frame:'20C8F0',icon:'F060D0',time:'B8E8F0',date:'508090'},'落日橙':{bg:'1A1008',menu:'F0D0A8',frame:'FF8030',icon:'FFC060',time:'F0D0A8',date:'907050'},'樱花粉':{bg:'180E12',menu:'F0C8D8',frame:'F078A8',icon:'C8A0F0',time:'F0C8D8',date:'805060'},'薄荷绿':{bg:'0C1810',menu:'C8F0DC',frame:'40E090',icon:'80E0C0',time:'C8F0DC',date:'508070'}};"
"Object.keys(P).forEach(k=>document.getElementById('preset').insertAdjacentHTML('beforeend','<option>'+k+'</option>'));"
"function preset(k){if(!P[k])return;Object.keys(P[k]).forEach(c=>document.getElementById('col_'+c).value='#'+P[k][c]);}"
"function amd(d,o){if(o)return'once';if(d==127)return'day';if(d==62)return'work';if(d==65)return'wknd';return'cust';}"
"function almhtml(a,i){let m=amd(a.days,a.once),s='<div class=alarm><input id=hh_'+i+' type=number min=0 max=23 value='+a.hh+'><b>:</b><input id=mm_'+i+' type=number min=0 max=59 value='+a.mm+'><select id=md_'+i+'><option value=day'+(m=='day'?' selected':'')+'>每天</option><option value=work'+(m=='work'?' selected':'')+'>工作日</option><option value=wknd'+(m=='wknd'?' selected':'')+'>周末</option><option value=once'+(m=='once'?' selected':'')+'>一次性</option><option value=cust'+(m=='cust'?' selected':'')+'>自定义</option></select><span>';"
"['日','一','二','三','四','五','六'].forEach(function(d,k){s+='<label style=font-size:11px><input type=checkbox id=dw'+i+'_'+k+(a.days&(1<<k)?' checked':'')+'>'+d+'</label>';});"
"s+='<button onclick=almDel('+i+') style=padding:1px 6px;color:#ffb070>删</button></div>';return s;}"
"let AL=[];"
"function almRender(){let h=(AL||[]).map((a,i)=>almhtml(a,i)).join('');"
"h+='<div style=margin-top:6px><button onclick=almAdd() style=background:#2a5a8f;color:#fff;padding:6px 14px;border:0;border-radius:6px>+ 添加闹钟</button></div>';"
"document.getElementById('alarms').innerHTML=h;}"
"function almAdd(){if(AL.length>=16){alert('最多16个闹钟');return;}AL.push({en:1,hh:8,mm:0,days:127,once:0});almRender();}"
"function almDel(i){AL.splice(i,1);almRender();}"
"async function load(){let j=await (await fetch('/api/cfg')).json(),st=document.getElementById('st');"
"if(j.status.wifi){st.className='st ok';st.textContent='已连 WiFi · '+j.status.ip+' · 手机/电脑打开 http://'+j.status.ip+'/';}"
"else{st.className='st no';st.textContent='未连 WiFi: 先连热点「'+j.ap_ssid+'」(密码 '+j.ap_pass+'), 再开 http://192.168.4.1/';}"
"document.getElementById('ssid').value=j.wifi.ssid;document.getElementById('pass').value=j.wifi.pass;document.getElementById('city').value=j.city;document.getElementById('key').value=j.key||'';"
"document.getElementById('ins').value=(j.ins||[]).join('\\n');"
"document.getElementById('ans0').value=j.ans&&j.ans.c0||'';document.getElementById('ans1').value=j.ans&&j.ans.c1||'';document.getElementById('ans2').value=j.ans&&j.ans.c2||'';document.getElementById('ans3').value=j.ans&&j.ans.c3||'';"
"let uo=j.users.map(u=>'<option'+(u==j.user?' selected':'')+'>'+esc(u)+'</option>').join('');if(j.users.indexOf(j.user)<0)uo+='<option selected>'+esc(j.user)+'</option>';document.getElementById('user').innerHTML=uo;"
"document.getElementById('cols').innerHTML=COLS.map((c,i)=>'<span>'+CN[i]+'<input type=color id=col_'+c+' value=#'+j.colors[c]+'></span>').join('');"
"AL=(j.alarms||[]).filter(function(a){return a.en;});almRender();"
"document.getElementById('beep').checked=j.beep?1:0;document.getElementById('vol').value=j.vol;"
"document.getElementById('timeout').value=j.timeout;document.getElementById('on').value=j.oracle_n;document.getElementById('ow').value=j.oracle_win;"
"document.getElementById('cursor').value=j.cursor;"
"document.getElementById('gdef').value='#'+j.garble.def;document.getElementById('ggb').value='#'+j.garble.gb;"
"document.getElementById('gdl').value=j.garble.dl;document.getElementById('grv').value=j.garble.rv;document.getElementById('gfnt').value=j.garble.fnt;}"
"async function userAdd(){let n=document.getElementById('useradd').value.trim();if(!n)return;let r=await(await fetch('/api/user',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:n})})).json();if(r.ok){let s=document.getElementById('user');if(![...s.options].some(o=>o.value==n))s.insertAdjacentHTML('beforeend','<option>'+esc(n)+'</option>');s.value=n;document.getElementById('useradd').value='';}else{alert('添加失败');}}"
"async function save(){let cols={},alarms=[];"
"COLS.forEach(c=>cols[c]=document.getElementById('col_'+c).value.slice(1));"
"let na=document.querySelectorAll('#alarms .alarm').length;"
"for(let i=0;i<na;i++){let m=document.getElementById('md_'+i).value,days=127,once=0;"
"if(m=='work')days=62;else if(m=='wknd')days=65;else if(m=='once'){days=127;once=1;}"
"else if(m=='cust'){days=0;for(let k=0;k<7;k++)if(document.getElementById('dw'+i+'_'+k).checked)days|=1<<k;}"
"alarms.push({en:1,hh:+document.getElementById('hh_'+i).value,mm:+document.getElementById('mm_'+i).value,days:days,once:once});}"
"let body={colors:cols,ins:document.getElementById('ins').value,user:document.getElementById('user').value,alarms:alarms,"
"wifi:{ssid:document.getElementById('ssid').value,pass:document.getElementById('pass').value},"
"city:document.getElementById('city').value,key:document.getElementById('key').value,beep:document.getElementById('beep').checked?1:0,vol:+document.getElementById('vol').value,"
"timeout:+document.getElementById('timeout').value,oracle_n:+document.getElementById('on').value,oracle_win:+document.getElementById('ow').value,"
"cursor:+document.getElementById('cursor').value,"
"garble:{def:document.getElementById('gdef').value.slice(1),gb:document.getElementById('ggb').value.slice(1),dl:+document.getElementById('gdl').value,rv:+document.getElementById('grv').value,fnt:+document.getElementById('gfnt').value},"
"ans:{c0:document.getElementById('ans0').value,c1:document.getElementById('ans1').value,c2:document.getElementById('ans2').value,c3:document.getElementById('ans3').value}};"
"let j=await (await fetch('/api/cfg',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})).json();"
"document.getElementById('msg').textContent=j.ok?'✓ 已保存':'保存失败';}"
"async function beep(){await fetch('/api/beep',{method:'POST'});}"
"async function reboot(){if(confirm('确定重启 BB 机?')){await fetch('/api/reboot',{method:'POST'});document.getElementById('msg').textContent='重启中…';}}"
"async function clrwifi(){if(confirm('清除已存 WiFi 并回到配网模式?')){await fetch('/api/clearwifi',{method:'POST'});location.reload();}}"
"document.getElementById('scanwifi').onclick=async function(){let b=this;b.disabled=1;b.textContent='扫描中…';"
"let l=document.getElementById('wifilist');l.innerHTML='';l.style.display='block';"
"try{let j=await (await fetch('/api/scan',{method:'POST'})).json();"
"if(!j.wifi_list||!j.wifi_list.length){l.innerHTML='<div style=padding:8px;color:#ffb070>没扫到 WiFi</div>';}"
"else j.wifi_list.forEach(function(w){let d=document.createElement('div');"
"d.style.cssText='padding:7px 10px;border-bottom:1px solid #2a4a2a;cursor:pointer;display:flex;justify-content:space-between;font-size:14px';"
"d.innerHTML=(w.encrypted?'🔒 ':'🌐 ')+esc(w.ssid)+'<span style=color:#8a9a8a>'+w.rssi+'dBm</span>';"
"d.onclick=function(){document.getElementById('ssid').value=w.ssid;document.getElementById('pass').focus();};l.appendChild(d);});}"
"catch(e){l.innerHTML='<div style=padding:8px;color:#ffb070>扫描失败</div>';}"
"b.disabled=0;b.textContent='重新扫描 WiFi';};"
"async function sendcmd(){let t=document.getElementById('sendtxt').value;if(!t)return;"
"let j=await (await fetch('/api/send',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({cmd:t})})).json();"
"document.getElementById('msg2').textContent='已发送, 设备正在显示';}"
"async function gacha(){let j=await (await fetch('/api/gacha')).json();"
"document.getElementById('gtotal').textContent=j.total;document.getElementById('gowned').textContent=j.owned;"
"let h='';j.sinners.forEach(function(s){"
"h+='<div style=color:#8fd48a;font-size:13px;margin:4px 0 2px>'+esc(s.name)+' '+s.owned+'/'+s.total+'</div>';"
"s.items.forEach(function(it){h+='<div style=padding-left:14px;font-size:12px;color:'+(it.owned?'#c8e0c0':'#6a8a6a')+'>'+(it.owned?'★ ':'▢ ')+esc(it.name)+'</div>';});});"
"document.getElementById('gacha').innerHTML=h;}"
"gacha();"
"async function todoLoad(){let j=await (await fetch('/api/todo')).json();let h='';(j.todos||[]).forEach(function(t,i){h+='<div style=padding:6px 4px;border-bottom:1px solid #2a4a2a;display:flex;justify-content:space-between;align-items:center>'+(t.done?'<span style=color:#6a8a6a>✓ '+esc(t.text)+'</span>':'<span>'+esc(t.text)+'</span>')+'<span><button onclick=todoToggle('+i+') style=padding:3px 8px>'+(t.done?'恢复':'PASS')+'</button><button onclick=todoDel('+i+') style=padding:3px 8px>删除</button></span></div>';});document.getElementById('todolist').innerHTML=h||'<div style=color:#6a8a6a>还没有待办</div>';}"
"async function todoAdd(){let t=document.getElementById('txt').value;if(!t)return;await fetch('/api/todo',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({op:'add',text:t})});document.getElementById('txt').value='';todoLoad();}"
"async function todoToggle(i){await fetch('/api/todo',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({op:'toggle',idx:i})});todoLoad();}"
"async function todoDel(i){await fetch('/api/todo',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({op:'del',idx:i})});todoLoad();}"
"async function todoClear(){if(confirm('清空全部待办?')){await fetch('/api/todo',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({op:'clear'})});todoLoad();}}"
"async function devst(){let j=await (await fetch('/api/status')).json();"
"document.getElementById('devst').innerHTML='电量 '+(j.battery<0?'未接电池':j.battery+'%')+' · 空闲堆 '+j.heap+'B<br>开机 '+j.boot+' 次 · IP '+j.ip+'<br>WiFi '+(j.wifi?'已连':'未连')+' · '+esc(j.ssid)+'<br>时间 '+j.date+' '+j.time+'<br>天气 '+esc(j.weather);}"
"devst();"
"todoLoad();"
"load();</script></body></html>";

/* ================= API 处理 ================= */

static esp_err_t web_handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, web_page, HTTPD_RESP_USE_STRLEN);
}

/* GET /api/cfg */
static esp_err_t web_api_cfg_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *colors = cJSON_CreateObject();
    cJSON *ins = cJSON_CreateArray();
    cJSON *alarms = cJSON_CreateArray();
    uint8_t n, i;
    char hx[8];

    for (i = 0; i < WEB_COLOR_N; i++)
    {
        rgb565_to_hex(*web_colors[i].color, hx);
        cJSON_AddStringToObject(colors, web_colors[i].name, hx);
    }
    cJSON_AddItemToObject(root, "colors", colors);

    {
        const char *const *pres = INS_Presets(&n);
        for (i = 0; i < n; i++)
        {
            cJSON_AddItemToArray(ins, cJSON_CreateString(pres[i]));
        }
    }
    cJSON_AddItemToObject(root, "ins", ins);

    /* 答案之书自定义答案(每类一行一条, 内置答案固定不可改) */
    {
        cJSON *ans = cJSON_CreateObject();
        cJSON_AddStringToObject(ans, "c0", ANS_Custom(0));   /* 回答 */
        cJSON_AddStringToObject(ans, "c1", ANS_Custom(1));   /* 吃什么 */
        cJSON_AddStringToObject(ans, "c2", ANS_Custom(2));   /* 喝什么 */
        cJSON_AddStringToObject(ans, "c3", ANS_Custom(3));   /* 玩什么 */
        cJSON_AddItemToObject(root, "ans", ans);
    }

    {
        uint8_t max = ALM_Max();
        for (i = 0; i < max; i++)
        {
            uint8_t en, hh, mm, days, once;
            cJSON *a = cJSON_CreateObject();
            ALM_GetSlot(i, &en, &hh, &mm, &days, &once);
            cJSON_AddNumberToObject(a, "en", en);
            cJSON_AddNumberToObject(a, "hh", hh);
            cJSON_AddNumberToObject(a, "mm", mm);
            cJSON_AddNumberToObject(a, "days", days);
            cJSON_AddNumberToObject(a, "once", once);
            cJSON_AddItemToArray(alarms, a);
        }
    }
    cJSON_AddItemToObject(root, "alarms", alarms);

    /* 连接/系统 */
    {
        cJSON *wifi = cJSON_CreateObject();
        cJSON_AddStringToObject(wifi, "ssid", NET_GetSsid());
        /* 凭据脱敏: 密码/key 不回传明文; 已配置->掩码"********"(保存时保持), 未配置->空 */
        cJSON_AddStringToObject(wifi, "pass", (NET_GetPass()[0]) ? WEB_SECRET_MASK : "");
        cJSON_AddItemToObject(root, "wifi", wifi);
        cJSON_AddStringToObject(root, "city", NET_GetCity());
        cJSON_AddStringToObject(root, "key", (NET_GetKey()[0]) ? WEB_SECRET_MASK : "");
        cJSON_AddStringToObject(root, "user", INS_UserName());
        {
            /* 使用者列表(与设备端子菜单一致, 网页下拉选择) */
            uint8_t un = 0, k;
            const char *const *ul = UI_UserList(&un);
            cJSON *uarr = cJSON_AddArrayToObject(root, "users");
            for (k = 0; k < un; k++) cJSON_AddItemToArray(uarr, cJSON_CreateString(ul[k]));
        }
        cJSON_AddNumberToObject(root, "beep", SET_Beep());
        cJSON_AddNumberToObject(root, "vol", SET_Vol());
        cJSON_AddNumberToObject(root, "timeout", SET_TimeoutSec());
        cJSON_AddNumberToObject(root, "oracle_n", SET_OracleN());
        cJSON_AddNumberToObject(root, "oracle_win", SET_OracleWin());
        cJSON_AddNumberToObject(root, "cursor", UI_GetCursorStyle());
        cJSON *status = cJSON_CreateObject();
        cJSON_AddNumberToObject(status, "wifi", NET_WifiOk() ? 1 : 0);
        cJSON_AddStringToObject(status, "ip", NET_IpStr());
        cJSON_AddItemToObject(root, "status", status);
        cJSON_AddStringToObject(root, "ap_ssid", NET_GetApSsid());
        cJSON_AddStringToObject(root, "ap_pass", NET_GetApPass());
    }

    /* 破译参数 */
    {
        uint16_t def, gb, dl, rv;
        char h1[8], h2[8];
        cJSON *garble = cJSON_CreateObject();
        INS_GetParams(&def, &gb, &dl, &rv);
        rgb565_to_hex(def, h1);
        rgb565_to_hex(gb, h2);
        cJSON_AddStringToObject(garble, "def", h1);
        cJSON_AddStringToObject(garble, "gb", h2);
        cJSON_AddNumberToObject(garble, "dl", dl);
        cJSON_AddNumberToObject(garble, "rv", rv);
        cJSON_AddNumberToObject(garble, "fnt", INS_Font());   /* 破译字号 0/1/2 = 16/24/32px */
        cJSON_AddItemToObject(root, "garble", garble);
    }

    {
        char *s = cJSON_PrintUnformatted(root);
        if (s)
        {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, s);
            cJSON_free(s);
        }
        else
        {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

/* POST /api/cfg */
/* ================= 配置应用(web_api_cfg_post 按项分发) ================= */
static int web_apply_colors(cJSON *root)
{
    cJSON *colors = cJSON_GetObjectItem(root, "colors");
    uint8_t i;
    if (!colors) return 1;
    if (!cJSON_IsObject(colors)) return 0;
    for (i = 0; i < WEB_COLOR_N; i++)
    {
        cJSON *it = cJSON_GetObjectItem(colors, web_colors[i].name);
        if (it && (!cJSON_IsString(it) || !web_hex_color_valid(it->valuestring))) return 0;
    }
    for (i = 0; i < WEB_COLOR_N; i++)
    {
        cJSON *it = cJSON_GetObjectItem(colors, web_colors[i].name);
        if (it) *web_colors[i].color = hex_to_rgb565(it->valuestring);
    }
    web_colors_save();
    return 1;
}

static int web_apply_ins(cJSON *root)
{
    cJSON *ins = cJSON_GetObjectItem(root, "ins");
    if (!ins) return 1;
    if (!cJSON_IsString(ins) || !web_ins_text_valid(ins->valuestring)) return 0;
    INS_PresetsFromText(ins->valuestring);
    return 1;
}

/* 答案之书整类答案(内置+自定义全量): {ans:{c0:"..",c1:"..",c2:"..",c3:".."}} 每行一条 */
static int web_ans_text_valid(const char *s)
{
    const char *p = s, *nl;
    uint8_t n = 0;
    if (!web_utf8_valid(s, ANS_TOTAL_MAX * ANS_LINE_MAX)) return 0;
    while (*p)
    {
        size_t len;
        nl = strchr(p, '\n');
        len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= ANS_LINE_MAX) return 0;   /* 单条答案过长 */
        if (len > 0)
        {
            n++;
            if (n > ANS_TOTAL_MAX) return 0;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 1;
}

static int web_apply_ans(cJSON *root)
{
    cJSON *ans = cJSON_GetObjectItem(root, "ans");
    uint8_t c;
    if (!ans) return 1;
    if (!cJSON_IsObject(ans)) return 0;
    for (c = 0; c < ANS_CAT_N; c++)
    {
        static const char *keys[ANS_CAT_N] = { "c0", "c1", "c2", "c3" };
        cJSON *it = cJSON_GetObjectItem(ans, keys[c]);
        if (!it) continue;
        if (!cJSON_IsString(it) || !web_ans_text_valid(it->valuestring)) return 0;
        ANS_FromText(c, it->valuestring);
    }
    return 1;
}

static int web_apply_alarms(cJSON *root)
{
    cJSON *alarms = cJSON_GetObjectItem(root, "alarms");
    int n;
    uint8_t max, i;
    if (!alarms) return 1;
    if (!cJSON_IsArray(alarms)) return 0;
    n = cJSON_GetArraySize(alarms);
    max = ALM_Max();
    if (n < 0 || n > max) return 0;
    for (i = 0; i < (uint8_t)n; i++)
    {
        cJSON *a = cJSON_GetArrayItem(alarms, i);
        if (!cJSON_IsObject(a)) return 0;
        cJSON *en = cJSON_GetObjectItem(a, "en");
        cJSON *hh = cJSON_GetObjectItem(a, "hh");
        cJSON *mm = cJSON_GetObjectItem(a, "mm");
        cJSON *days = cJSON_GetObjectItem(a, "days");
        cJSON *once = cJSON_GetObjectItem(a, "once");
        if (!cJSON_IsNumber(en) || !cJSON_IsNumber(hh) || !cJSON_IsNumber(mm)) return 0;
        if ((en->valueint != 0 && en->valueint != 1) ||
            hh->valueint < 0 || hh->valueint > 23 ||
            mm->valueint < 0 || mm->valueint > 59) return 0;
        if (days && (!cJSON_IsNumber(days) || days->valueint < 0 || days->valueint > 127)) return 0;
        if (once && (!cJSON_IsNumber(once) || (once->valueint != 0 && once->valueint != 1))) return 0;
    }
    for (i = 0; i < (uint8_t)n; i++)
    {
        cJSON *a = cJSON_GetArrayItem(alarms, i);
        cJSON *en = cJSON_GetObjectItem(a, "en");
        cJSON *hh = cJSON_GetObjectItem(a, "hh");
        cJSON *mm = cJSON_GetObjectItem(a, "mm");
        cJSON *days = cJSON_GetObjectItem(a, "days");
        cJSON *once = cJSON_GetObjectItem(a, "once");
        ALM_SetSlot(i, (uint8_t)(en->valueint ? 1 : 0),
                    (uint8_t)hh->valueint, (uint8_t)mm->valueint,
                    days ? (uint8_t)days->valueint : 0x7F,
                    once ? (uint8_t)(once->valueint ? 1 : 0) : 0);
    }
    for (i = (uint8_t)n; i < max; i++)   /* 网页只列已设闹钟: 未列出的槽全部关闭 */
    {
        ALM_SetSlot(i, 0, 0, 0, 0x7F, 0);
    }
    return 1;
}

static int web_apply_net(cJSON *root)
{
    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    const char *ssid = NULL, *pass = NULL;
    if (wifi)
    {
        if (!cJSON_IsObject(wifi)) return 0;
        cJSON *s = cJSON_GetObjectItem(wifi, "ssid");
        cJSON *p = cJSON_GetObjectItem(wifi, "pass");
        if (!s || !p || !cJSON_IsString(s) || !cJSON_IsString(p)) return 0;
        if (s->valuestring[0] == '\0' || !web_utf8_valid(s->valuestring, 32)) return 0;
        if (!web_utf8_valid(p->valuestring, 64)) return 0;
        ssid = s->valuestring;
        pass = p->valuestring;
    }
    cJSON *city = cJSON_GetObjectItem(root, "city");
    if (city)
    {
        if (!cJSON_IsString(city) || !web_utf8_valid(city->valuestring, 23)) return 0;
        if (city->valuestring[0]) NET_SetCity(city->valuestring);
    }
    cJSON *key = cJSON_GetObjectItem(root, "key");
    if (key)
    {
        if (!cJSON_IsString(key) || !web_utf8_valid(key->valuestring, 47)) return 0;
        if (strcmp(key->valuestring, WEB_SECRET_MASK) != 0)
            NET_SetKey(key->valuestring);   /* 掩码=保持原 key; 空=不配置天气 */
    }
    if (ssid)
    {
        /* 掩码密码=保持不变: 仅当用户明确输入新密码(覆盖)时才改; 空=清空密码(开放网络) */
        if (strcmp(pass, WEB_SECRET_MASK) != 0) NET_SetWifi(ssid, pass);
        else NET_SetWifi(ssid, NET_GetPass());   /* 密码未动: 沿用已存密码(兼容只换 SSID) */
    }
    return 1;
}

static int web_apply_user(cJSON *root)
{
    cJSON *user = cJSON_GetObjectItem(root, "user");
    if (!user) return 1;
    if (!cJSON_IsString(user) || user->valuestring[0] == '\0') return 0;
    if (!web_utf8_valid(user->valuestring, INS_USER_NAME_MAX - 1) ||
        strchr(user->valuestring, '\n') || strchr(user->valuestring, '\r')) return 0;
    INS_SetUserName(user->valuestring);
    return 1;
}

static int web_apply_sound(cJSON *root)
{
    cJSON *beep = cJSON_GetObjectItem(root, "beep");
    if (beep)
    {
        if (!cJSON_IsNumber(beep) || (beep->valueint != 0 && beep->valueint != 1)) return 0;
        SET_SetBeep((uint8_t)(beep->valueint ? 1 : 0));
    }
    cJSON *vol = cJSON_GetObjectItem(root, "vol");
    if (vol)
    {
        if (!cJSON_IsNumber(vol) || vol->valueint < 0 || vol->valueint > 100) return 0;
        SET_SetVol((uint8_t)vol->valueint);
    }
    return 1;
}

static int web_apply_timeout(cJSON *root)
{
    cJSON *timeout = cJSON_GetObjectItem(root, "timeout");
    if (timeout)
    {
        int v;
        if (!cJSON_IsNumber(timeout)) return 0;
        v = timeout->valueint;
        if (v != 0 && v != 30 && v != 60 && v != 300) return 0;
        SET_SetTimeout((uint16_t)v);
    }
    cJSON *on = cJSON_GetObjectItem(root, "oracle_n");
    if (on)
    {
        int v;
        if (!cJSON_IsNumber(on)) return 0;
        v = on->valueint;
        if (v != 0 && v != 1 && v != 3 && v != 5 && v != 9) return 0;
        SET_SetOracleN((uint8_t)v);
    }
    cJSON *ow = cJSON_GetObjectItem(root, "oracle_win");
    if (ow)
    {
        if (!cJSON_IsNumber(ow) || ow->valueint < 0 || ow->valueint > 3) return 0;
        SET_SetOracleWin((uint8_t)ow->valueint);
    }
    return 1;
}

static int web_apply_cursor(cJSON *root)
{
    cJSON *cur = cJSON_GetObjectItem(root, "cursor");
    if (!cur) return 1;
    if (!cJSON_IsNumber(cur) || cur->valueint < 0 || cur->valueint >= UI_CURSOR_N) return 0;
    SET_SetCursor((uint8_t)cur->valueint);
    return 1;
}

static int web_apply_decode(cJSON *root)
{
    cJSON *garble = cJSON_GetObjectItem(root, "garble");
    if (!garble) return 1;
    if (!cJSON_IsObject(garble)) return 0;
    {
        cJSON *def = cJSON_GetObjectItem(garble, "def");
        cJSON *gb  = cJSON_GetObjectItem(garble, "gb");
        cJSON *dl  = cJSON_GetObjectItem(garble, "dl");
        cJSON *rv  = cJSON_GetObjectItem(garble, "rv");
        cJSON *fnt = cJSON_GetObjectItem(garble, "fnt");
        if (def || gb)
        {
            if (!def || !gb || !cJSON_IsString(def) || !cJSON_IsString(gb) ||
                !web_hex_color_valid(def->valuestring) || !web_hex_color_valid(gb->valuestring)) return 0;
        }
        if (dl && (!cJSON_IsNumber(dl) || dl->valueint < 5)) return 0;
        if (rv && (!cJSON_IsNumber(rv) || rv->valueint < 10)) return 0;
        if (fnt && (!cJSON_IsNumber(fnt) || fnt->valueint < 0 || fnt->valueint > 2)) return 0;
        if (def && gb)
        {
            INS_SetParams((uint16_t)hex_to_rgb565(def->valuestring),
                          (uint16_t)hex_to_rgb565(gb->valuestring),
                          dl ? (uint16_t)dl->valueint : 18,
                          rv ? (uint16_t)rv->valueint : 80);
        }
        if (fnt) INS_SetFont((uint8_t)fnt->valueint);   /* 破译字号(INS_SetFont 内部钳位+存NVS) */
    }
    return 1;
}

static esp_err_t web_api_cfg_post(httpd_req_t *req)
{
    int total = req->content_len, recvd = 0;
    char *buf;

    if (total <= 0 || total > 32768)   /* 含指令库+答案文本框+闹钟, 上限提到 32KB */
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad len");
        return ESP_OK;
    }
    buf = malloc((size_t)total + 1);
    if (!buf)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        return ESP_OK;
    }
    while (recvd < total)
    {
        int r = httpd_req_recv(req, buf + recvd, (size_t)(total - recvd));
        if (r <= 0)
        {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv fail");
            return ESP_OK;
        }
        recvd += r;
    }
    buf[recvd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    /* 按配置项逐个应用(各 web_apply_* 见上文; 非法字段返回400) */
    if (!web_apply_colors(root) || !web_apply_ins(root) || !web_apply_ans(root) ||
        !web_apply_alarms(root) ||
        !web_apply_net(root) || !web_apply_user(root) || !web_apply_sound(root) ||
        !web_apply_timeout(root) || !web_apply_cursor(root) || !web_apply_decode(root))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid field");
        return ESP_OK;
    }

    cJSON_Delete(root);
    web_dirty = 1;   /* 通知 ui_task 重绘主界面(主题色/使用者实时生效) */
    ESP_LOGI(TAG, "config applied");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":1}");
    return ESP_OK;
}

/* 扫描附近 WiFi(供配置页点击选择, 免手输 SSID) */
static esp_err_t web_api_scan(httpd_req_t *req)
{
    enum { SCAN_MAX = 20 };
    char ssids[SCAN_MAX][33];
    int8_t rssi[SCAN_MAX];
    uint8_t enc[SCAN_MAX];
    uint8_t n = NET_ScanWifi(SCAN_MAX, ssids, rssi, enc), i;
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "wifi_list");
    for (i = 0; i < n; i++)
    {
        cJSON *w = cJSON_CreateObject();
        cJSON_AddStringToObject(w, "ssid", ssids[i]);
        cJSON_AddNumberToObject(w, "rssi", rssi[i]);
        cJSON_AddNumberToObject(w, "encrypted", enc[i]);
        cJSON_AddItemToArray(arr, w);
    }
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    if (out) cJSON_free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

/* 抽卡图鉴: 按罪人分组返回全部人格与已抽标记(★=已抽), 供网页图鉴展示 */
static esp_err_t web_api_gacha(httpd_req_t *req)
{
    uint8_t s, n = GACHA_CoinSinnerN();
    uint16_t total = GACHA_CoinTotal();
    cJSON *root = cJSON_CreateObject();
    cJSON *sinners = cJSON_AddArrayToObject(root, "sinners");
    cJSON_AddNumberToObject(root, "total", total);
    cJSON_AddNumberToObject(root, "owned", GACHA_CoinOwnedCount());
    for (s = 0; s < n; s++)
    {
        uint16_t off = GACHA_CoinSinnerOff(s);
        uint16_t cnt = GACHA_CoinSinnerCount(s);
        uint16_t i, ow = 0;
        cJSON *so = cJSON_CreateObject();
        cJSON *items = cJSON_AddArrayToObject(so, "items");
        cJSON_AddStringToObject(so, "name", GACHA_CoinSinnerName(s));
        cJSON_AddNumberToObject(so, "total", cnt);
        for (i = 0; i < cnt; i++)
        {
            cJSON *it = cJSON_CreateObject();
            uint8_t owned = GACHA_CoinOwned(off + i);
            if (owned) ow++;
            cJSON_AddStringToObject(it, "name", GACHA_CoinName(off + i));
            cJSON_AddNumberToObject(it, "owned", owned);
            cJSON_AddItemToArray(items, it);
        }
        cJSON_AddNumberToObject(so, "owned", ow);
        cJSON_AddItemToArray(sinners, so);
    }
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    if (out) cJSON_free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

/* 设备状态(网页状态页): 电量/堆/开机次数/IP/WiFi/时间/天气 */
static esp_err_t web_api_status(httpd_req_t *req)
{
    uint8_t pct = BAT_GetPct();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "battery", (pct <= 100) ? pct : -1);
    cJSON_AddNumberToObject(root, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "boot", SET_BootCount());
    cJSON_AddStringToObject(root, "ip", NET_IpStr());
    cJSON_AddStringToObject(root, "ssid", NET_GetSsid());
    cJSON_AddNumberToObject(root, "wifi", NET_WifiOk() ? 1 : 0);
    cJSON_AddStringToObject(root, "date", NET_DateStr());
    cJSON_AddStringToObject(root, "time", NET_TimeStr());
    cJSON_AddStringToObject(root, "weather", NET_WeatherStr() ? NET_WeatherStr() : "暂无");
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    if (out) cJSON_free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

/* 待办列表: 返回 {todos:[{text,done},...]} */
static esp_err_t web_api_todo_get(httpd_req_t *req)
{
    uint8_t n = TODO_Count(), i;
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "todos");
    for (i = 0; i < n; i++)
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "text", TODO_Text(i));
        cJSON_AddNumberToObject(t, "done", TODO_Done(i));
        cJSON_AddItemToArray(arr, t);
    }
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    if (out) cJSON_free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

/* 待办操作: {op:"add",text} | {op:"toggle",idx} | {op:"del",idx} | {op:"clear"} */
static esp_err_t web_api_todo_post(httpd_req_t *req)
{
    int total = req->content_len, recvd = 0;
    char *buf;
    cJSON *root, *op, *text, *idx;
    if (total <= 0 || total > 2048)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad len");
        return ESP_OK;
    }
    buf = malloc((size_t)total + 1);
    if (!buf)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        return ESP_OK;
    }
    while (recvd < total)
    {
        int r = httpd_req_recv(req, buf + recvd, (size_t)(total - recvd));
        if (r <= 0)
        {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
            return ESP_OK;
        }
        recvd += r;
    }
    buf[total] = '\0';
    root = cJSON_Parse(buf);
    free(buf);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }
    op = cJSON_GetObjectItem(root, "op");
    if (!op || !cJSON_IsString(op))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad op");
        return ESP_OK;
    }
    if (strcmp(op->valuestring, "add") == 0)
    {
        text = cJSON_GetObjectItem(root, "text");
        if (!text || !cJSON_IsString(text) || text->valuestring[0] == '\0' ||
            !web_utf8_valid(text->valuestring, TODO_TEXT_MAX - 1) ||
            strchr(text->valuestring, '\n') || strchr(text->valuestring, '\r'))
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad text");
            return ESP_OK;
        }
        TODO_Add(text->valuestring);
    }
    else if (strcmp(op->valuestring, "toggle") == 0)
    {
        idx = cJSON_GetObjectItem(root, "idx");
        if (!idx || !cJSON_IsNumber(idx) || idx->valueint < 0 || idx->valueint >= TODO_Count())
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad idx");
            return ESP_OK;
        }
        TODO_Toggle((uint8_t)idx->valueint);
    }
    else if (strcmp(op->valuestring, "del") == 0)
    {
        idx = cJSON_GetObjectItem(root, "idx");
        if (!idx || !cJSON_IsNumber(idx) || idx->valueint < 0 || idx->valueint >= TODO_Count())
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad idx");
            return ESP_OK;
        }
        TODO_Del((uint8_t)idx->valueint);
    }
    else if (strcmp(op->valuestring, "clear") == 0)
    {
        TODO_Clear();
    }
    else
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad op");
        return ESP_OK;
    }
    web_dirty = 1;   /* 待办改了: 若设备在待办界面, ui_task 刷新列表 */
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":1}");
    return ESP_OK;
}

/* ================= 下发指令(网页 -> 设备破译显示) ================= */
static char web_pending_cmd[96];   /* 下发指令上限 96B: 避免拼"致使用者名"后超出破译缓冲(130B)截断 */
static volatile uint8_t web_pending_flag = 0;

/* POST /api/send: 接收 {cmd:"..."} 缓存, 由 ui_task 取出用乱码破译显示 */
static esp_err_t web_api_send(httpd_req_t *req)
{
    int total = req->content_len, recvd = 0;
    char *buf;
    cJSON *root, *cmd;
    if (total <= 0 || total > 2048)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad len");
        return ESP_OK;
    }
    buf = malloc((size_t)total + 1);
    if (!buf)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        return ESP_OK;
    }
    while (recvd < total)
    {
        int r = httpd_req_recv(req, buf + recvd, (size_t)(total - recvd));
        if (r <= 0)
        {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
            return ESP_OK;
        }
        recvd += r;
    }
    buf[total] = '\0';
    root = cJSON_Parse(buf);
    free(buf);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }
    cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cmd || !cJSON_IsString(cmd) || cmd->valuestring[0] == '\0' ||
        !web_utf8_valid(cmd->valuestring, sizeof(web_pending_cmd) - 1))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad cmd");
        return ESP_OK;
    }
    strncpy(web_pending_cmd, cmd->valuestring, sizeof(web_pending_cmd) - 1);
    web_pending_cmd[sizeof(web_pending_cmd) - 1] = '\0';
    web_pending_flag = 1;
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":1}");
    return ESP_OK;
}

/* ui_task 取出待显示指令(取走后清空), 返回 1=有待显示 */
uint8_t WEB_TakeCmd(char *buf, size_t n)
{
    if (!web_pending_flag)
    {
        return 0;
    }
    if (n > 0)
    {
        strncpy(buf, web_pending_cmd, n - 1);
        buf[n - 1] = '\0';
    }
    web_pending_flag = 0;
    return 1;
}

/* 试响: 蜂鸣器响一下(验证蜂鸣/音量) */
static esp_err_t web_api_beep(httpd_req_t *req)
{
    INS_BeepTimes(1);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":1}");
    return ESP_OK;
}

/* 重启设备 */
static esp_err_t web_api_reboot(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":1}");
    vTaskDelay(200 / portTICK_PERIOD_MS);
    esp_restart();
    return ESP_OK;
}

/* 添加使用者(网页端「添加」按钮): 收 {name}, 加入列表并持久化(NVS, 同指令库模式) */
static esp_err_t web_api_user_add(httpd_req_t *req)
{
    int total = req->content_len, recvd = 0;
    char *buf;
    if (total <= 0 || total > 512)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad len");
        return ESP_OK;
    }
    buf = malloc((size_t)total + 1);
    if (!buf)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        return ESP_OK;
    }
    while (recvd < total)
    {
        int r = httpd_req_recv(req, buf + recvd, (size_t)(total - recvd));
        if (r <= 0) { free(buf); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_OK; }
        recvd += r;
    }
    buf[recvd] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    uint8_t ok = 0;
    if (root)
    {
        cJSON *name = cJSON_GetObjectItem(root, "name");
        if (name && cJSON_IsString(name) && name->valuestring[0] &&
            web_utf8_valid(name->valuestring, UI_USER_NAME_MAX - 1) &&
            !strchr(name->valuestring, '\n') && !strchr(name->valuestring, '\r'))
        {
            ok = UI_UserAdd(name->valuestring);
        }
        cJSON_Delete(root);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":1}" : "{\"ok\":0}");
    return ESP_OK;
}

/* 清除已存 WiFi: 回纯 AP 配网模式(配置页"清除"按钮) */
static esp_err_t web_api_clearwifi(httpd_req_t *req)
{
    NET_ClearWifi();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":1}");
    return ESP_OK;
}

/* 手机连上热点的 captive portal 探测路径 -> 直接返回配置页(自动弹出, 免手输IP) */
static esp_err_t web_captive(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, web_page);
    return ESP_OK;
}

/* ================= 入口 ================= */
void WEB_Init(void)
{
    web_colors_load();   /* 加载持久化主题色 */

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 16384;          /* 大栈: 解析 + 大JSON响应 */
    cfg.max_uri_handlers = 24;       /* 主13 + captive portal 探测 7 + 余量 */
    cfg.max_open_sockets = 4;        /* 单手机够用, 少占堆 */
    cfg.lru_purge_enable = true;
    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "httpd start, free heap=%u", (unsigned)esp_get_free_heap_size());
    if (httpd_start(&server, &cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd start FAILED");
        return;
    }
    ESP_LOGI(TAG, "httpd started OK");
    httpd_uri_t r1 = { .uri = "/",           .method = HTTP_GET,  .handler = web_handler_root,   .user_ctx = NULL };
    httpd_uri_t r2 = { .uri = "/api/cfg",    .method = HTTP_GET,  .handler = web_api_cfg_get,    .user_ctx = NULL };
    httpd_uri_t r3 = { .uri = "/api/cfg",    .method = HTTP_POST, .handler = web_api_cfg_post,   .user_ctx = NULL };
    httpd_uri_t r4 = { .uri = "/api/beep",   .method = HTTP_POST, .handler = web_api_beep,       .user_ctx = NULL };
    httpd_uri_t r5 = { .uri = "/api/reboot", .method = HTTP_POST, .handler = web_api_reboot,     .user_ctx = NULL };
    httpd_uri_t r6 = { .uri = "/api/scan",   .method = HTTP_POST, .handler = web_api_scan,       .user_ctx = NULL };
    httpd_uri_t r7 = { .uri = "/api/send",   .method = HTTP_POST, .handler = web_api_send,       .user_ctx = NULL };
    httpd_uri_t r8 = { .uri = "/api/gacha",  .method = HTTP_GET,  .handler = web_api_gacha,       .user_ctx = NULL };
    httpd_uri_t r9 = { .uri = "/api/todo",   .method = HTTP_GET,  .handler = web_api_todo_get,    .user_ctx = NULL };
    httpd_uri_t r10 = { .uri = "/api/todo",  .method = HTTP_POST, .handler = web_api_todo_post,   .user_ctx = NULL };
    httpd_uri_t r11 = { .uri = "/api/status", .method = HTTP_GET, .handler = web_api_status,      .user_ctx = NULL };
    httpd_uri_t r12 = { .uri = "/api/user",   .method = HTTP_POST, .handler = web_api_user_add,   .user_ctx = NULL };
    httpd_uri_t r13 = { .uri = "/api/clearwifi", .method = HTTP_POST, .handler = web_api_clearwifi, .user_ctx = NULL };
    httpd_register_uri_handler(server, &r1);
    httpd_register_uri_handler(server, &r2);
    httpd_register_uri_handler(server, &r3);
    httpd_register_uri_handler(server, &r4);
    httpd_register_uri_handler(server, &r5);
    httpd_register_uri_handler(server, &r6);
    httpd_register_uri_handler(server, &r7);
    httpd_register_uri_handler(server, &r8);
    httpd_register_uri_handler(server, &r9);
    httpd_register_uri_handler(server, &r10);
    httpd_register_uri_handler(server, &r11);
    httpd_register_uri_handler(server, &r12);
    httpd_register_uri_handler(server, &r13);

    /* captive portal 探测路径(安卓/iOS/Windows): 全返回配置页, 手机连热点自动弹出 */
    {
        static const char *probes[] = {
            "/generate_204", "/connectivity-check.gstatic.com",
            "/library/test/success.html", "/hotspot-detect.html",
            "/ncsi.txt", "/connecttest.txt", "/gen_204"
        };
        uint8_t k;
        for (k = 0; k < sizeof(probes) / sizeof(probes[0]); k++)
        {
            httpd_uri_t p = { .uri = probes[k], .method = HTTP_GET, .handler = web_captive, .user_ctx = NULL };
            httpd_register_uri_handler(server, &p);
        }
    }
    ESP_LOGI(TAG, "config page: AP=%s -> http://192.168.4.1/  or  http://<router-ip>/", NET_GetSsid()[0] ? "STA" : "配网");
}
