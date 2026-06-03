// Tiny YOLO + tarayici web panosu (MJPEG benzeri /frame + JSON /data)
// Kartta: make -f makefile_web && ./tiny_yolo_web
// PC: http://192.168.2.99:8080

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <math.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <opencv2/opencv.hpp>
#include <dnndk/n2cube.h>

using namespace std;
using namespace cv;

#define CONF      0.35f
#define NMS_THRE  0.1f
#define YOLOKERNEL "tiny_yolo"
#define INPUTNODE  "conv2d_1_convolution"
#define WEB_PORT   8080

vector<string> outputs_node = {"conv2d_10_convolution", "conv2d_13_convolution"};
const int classification = 80;
const int anchor = 3;
vector<float> biases { 116,90, 156,198, 373,326, 30,61, 62,45, 59,119, 10,13, 16,30, 33,23 };

vector<string> class_names = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck",
    "boat", "traffic light", "fire hydrant", "stop sign", "parking meter", "bench",
    "bird", "cat", "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra",
    "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove",
    "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup", "fork",
    "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange", "broccoli",
    "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch", "potted plant",
    "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard",
    "cell phone", "microwave", "oven", "toaster", "sink", "refrigerator", "book",
    "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush"
};

struct WebDet {
    string label;
    float conf;
    int xmin, ymin, xmax, ymax;
};

static mutex g_mtx;
static vector<WebDet> g_dets;
static vector<uchar> g_jpeg;
static atomic<float> g_fps{0.f};
static atomic<bool> g_running{true};
static atomic<double> g_last_frame_mono{0.0};

static string json_escape(const string& s) {
    string o;
    for (char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else o += c;
    }
    return o;
}

static void set_input_image_fast(DPUTask* task, const Mat& img, const char* node) {
    int in_h = dpuGetInputTensorHeight(task, node);
    int in_w = dpuGetInputTensorWidth(task, node);
    int size = dpuGetInputTensorSize(task, node);
    int8_t* data = dpuGetInputTensorAddress(task, node);
    float scale = dpuGetInputTensorScale(task, node);

    float r = min((float)in_w / img.cols, (float)in_h / img.rows);
    int new_w = (int)(img.cols * r);
    int new_h = (int)(img.rows * r);
    int dx = (in_w - new_w) / 2;
    int dy = (in_h - new_h) / 2;

    static Mat canvas;
    if (canvas.empty() || canvas.cols != in_w || canvas.rows != in_h)
        canvas = Mat(in_h, in_w, CV_8UC3, Scalar(128, 128, 128));
    else
        canvas.setTo(Scalar(128, 128, 128));

    Mat roi = canvas(Rect(dx, dy, new_w, new_h));
    resize(img, roi, Size(new_w, new_h), 0, 0, INTER_LINEAR);
    cvtColor(canvas, canvas, COLOR_BGR2RGB);
    static Mat quant;
    canvas.convertTo(quant, CV_8SC3, scale / 255.0f);
    memcpy(data, quant.data, size);
}

static void get_output(int8_t* dpuOut, int sizeOut, float scale,
                       int oc, int oh, int ow, vector<float>& result) {
    vector<int8_t> nums(sizeOut);
    memcpy(nums.data(), dpuOut, sizeOut);
    for (int a = 0; a < oc; ++a)
        for (int b = 0; b < oh; ++b)
            for (int c = 0; c < ow; ++c) {
                int offset = b * oc * ow + c * oc + a;
                result[a * oh * ow + b * ow + c] = nums[offset] * scale;
            }
}

static inline float sigmoid(float p) { return 1.f / (1.f + expf(-p)); }

static inline float overlap(float x1, float w1, float x2, float w2) {
    float left = max(x1 - w1 * 0.5f, x2 - w2 * 0.5f);
    float right = min(x1 + w1 * 0.5f, x2 + w2 * 0.5f);
    return right - left;
}

static inline float cal_iou(const vector<float>& box, const vector<float>& truth) {
    float w = overlap(box[0], box[2], truth[0], truth[2]);
    float h = overlap(box[1], box[3], truth[1], truth[3]);
    if (w < 0 || h < 0) return 0;
    float inter = w * h;
    float uni = box[2] * box[3] + truth[2] * truth[3] - inter;
    return inter / uni;
}

static vector<vector<float>> apply_nms(vector<vector<float>>& boxes, int classes, float thres) {
    vector<vector<float>> result;
    if (boxes.empty()) return result;
    vector<bool> class_has(classes, false);
    for (const auto& b : boxes)
        for (int k = 0; k < classes; ++k)
            if (b[6 + k] >= CONF) class_has[k] = true;

    vector<pair<int, float>> order(boxes.size());
    for (int k = 0; k < classes; ++k) {
        if (!class_has[k]) continue;
        for (size_t i = 0; i < boxes.size(); ++i) {
            order[i].first = (int)i;
            boxes[i][4] = (float)k;
            order[i].second = boxes[i][6 + k];
        }
        sort(order.begin(), order.end(),
             [](const pair<int, float>& a, const pair<int, float>& b) { return a.second > b.second; });
        vector<bool> exist(boxes.size(), true);
        for (size_t _i = 0; _i < boxes.size(); ++_i) {
            size_t i = order[_i].first;
            if (!exist[i]) continue;
            if (boxes[i][6 + k] < CONF) { exist[i] = false; continue; }
            result.push_back(boxes[i]);
            for (size_t _j = _i + 1; _j < boxes.size(); ++_j) {
                size_t j = order[_j].first;
                if (!exist[j]) continue;
                if (cal_iou(boxes[j], boxes[i]) >= thres) exist[j] = false;
            }
        }
    }
    return result;
}

static void correct_region_boxes(vector<vector<float>>& boxes, int n, int w, int h, int netw, int neth) {
    int new_w, new_h;
    if (((float)netw / w) < ((float)neth / h)) { new_w = netw; new_h = (h * netw) / w; }
    else { new_h = neth; new_w = (w * neth) / h; }
    for (int i = 0; i < n; ++i) {
        boxes[i][0] = (boxes[i][0] - (netw - new_w) / 2.f / netw) / ((float)new_w / netw);
        boxes[i][1] = (boxes[i][1] - (neth - new_h) / 2.f / neth) / ((float)new_h / neth);
        boxes[i][2] *= (float)netw / new_w;
        boxes[i][3] *= (float)neth / new_h;
    }
}

static void detect(vector<vector<float>>& boxes, const vector<float>& result,
                   int channel, int height, int width, int num, int sh, int sw) {
    int conf_box = 5 + classification;
    vector<vector<vector<float>>> swap(height * width,
        vector<vector<float>>(anchor, vector<float>(conf_box, 0)));
    for (int h = 0; h < height; ++h)
        for (int w = 0; w < width; ++w)
            for (int c = 0; c < channel; ++c) {
                int temp = c * height * width + h * width + w;
                swap[h * width + w][c / conf_box][c % conf_box] = result[temp];
            }
    for (int h = 0; h < height; ++h) {
        for (int w = 0; w < width; ++w) {
            for (int c = 0; c < anchor; ++c) {
                float obj_score = sigmoid(swap[h * width + w][c][4]);
                if (obj_score < CONF) continue;
                vector<float> box;
                box.push_back((w + sigmoid(swap[h * width + w][c][0])) / width);
                box.push_back((h + sigmoid(swap[h * width + w][c][1])) / height);
                box.push_back(expf(swap[h * width + w][c][2]) * biases[2 * c + anchor * 2 * num] / (float)sw);
                box.push_back(expf(swap[h * width + w][c][3]) * biases[2 * c + anchor * 2 * num + 1] / (float)sh);
                box.push_back(-1);
                box.push_back(obj_score);
                for (int p = 0; p < classification; ++p)
                    box.push_back(obj_score * sigmoid(swap[h * width + w][c][5 + p]));
                boxes.push_back(move(box));
            }
        }
    }
}

static void deal_web(DPUTask* task, Mat& img, int sw, int sh) {
    vector<vector<float>> boxes;
    for (size_t i = 0; i < outputs_node.size(); ++i) {
        const string& out = outputs_node[i];
        int channel = dpuGetOutputTensorChannel(task, out.c_str());
        int width = dpuGetOutputTensorWidth(task, out.c_str());
        int height = dpuGetOutputTensorHeight(task, out.c_str());
        int sizeOut = dpuGetOutputTensorSize(task, out.c_str());
        int8_t* dpuOut = dpuGetOutputTensorAddress(task, out.c_str());
        float scale = dpuGetOutputTensorScale(task, out.c_str());
        vector<float> result(sizeOut);
        get_output(dpuOut, sizeOut, scale, channel, height, width, result);
        detect(boxes, result, channel, height, width, (int)i, sh, sw);
    }
    correct_region_boxes(boxes, (int)boxes.size(), img.cols, img.rows, sw, sh);
    vector<vector<float>> res = apply_nms(boxes, classification, NMS_THRE);

    vector<WebDet> dets;
    float h = img.rows, w = img.cols;
    for (size_t i = 0; i < res.size(); ++i) {
        float xmin = (res[i][0] - res[i][2] * 0.5f) * w;
        float ymin = (res[i][1] - res[i][3] * 0.5f) * h;
        float xmax = (res[i][0] + res[i][2] * 0.5f) * w;
        float ymax = (res[i][1] + res[i][3] * 0.5f) * h;
        int cls = (int)res[i][4];
        float conf = res[i][5];
        const string& name = class_names[cls];

        WebDet d;
        d.label = name;
        d.conf = conf;
        d.xmin = (int)xmin; d.ymin = (int)ymin;
        d.xmax = (int)xmax; d.ymax = (int)ymax;
        dets.push_back(d);

        rectangle(img, Point((int)xmin, (int)ymin), Point((int)xmax, (int)ymax), Scalar(0, 200, 255), 2);
        char label[64];
        snprintf(label, sizeof(label), "%s %.0f%%", name.c_str(), conf * 100.f);
        int baseline = 0;
        Size ts = getTextSize(label, FONT_HERSHEY_SIMPLEX, 0.55, 2, &baseline);
        rectangle(img, Point((int)xmin, (int)ymin - ts.height - 8),
                    Point((int)xmin + ts.width + 4, (int)ymin), Scalar(0, 200, 255), FILLED);
        putText(img, label, Point((int)xmin + 2, (int)ymin - 4),
                FONT_HERSHEY_SIMPLEX, 0.55, Scalar(0, 0, 0), 2, LINE_AA);
    }

    vector<uchar> jpeg;
    vector<int> params = {IMWRITE_JPEG_QUALITY, 75};
    imencode(".jpg", img, jpeg, params);

    lock_guard<mutex> lk(g_mtx);
    g_dets = move(dets);
    g_jpeg = move(jpeg);
    g_last_frame_mono.store(chrono::steady_clock::now().time_since_epoch().count() / 1e9);
}

static const char* HTML_PAGE = R"HTML(<!DOCTYPE html>
<html lang="tr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PYNQ-Z2 Tiny YOLO</title>
<style>
:root{color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;font-family:'Segoe UI',system-ui,sans-serif;background:#0d1117;color:#e6edf3}
header{padding:12px 20px;background:#161b22;border-bottom:1px solid #30363d;display:flex;align-items:center;gap:14px;flex-wrap:wrap}
header h1{font-size:17px;margin:0;font-weight:600}
.logo{width:26px;height:26px;border-radius:6px;background:linear-gradient(135deg,#1f6feb,#a371f7);display:inline-flex;align-items:center;justify-content:center;font-size:14px;font-weight:800;color:#fff}
.badge{padding:4px 12px;border-radius:20px;font-size:12px;font-weight:600;background:#1a7f37;color:#fff}
.wrap{display:grid;grid-template-columns:340px 1fr;gap:0;height:calc(100vh - 53px)}
@media(max-width:900px){.wrap{grid-template-columns:1fr;height:auto}}
.panel{padding:16px;overflow-y:auto;border-right:1px solid #30363d}
.card{background:#161b22;border:1px solid #30363d;border-radius:10px;padding:14px;margin-bottom:12px}
.card h2{font-size:11px;text-transform:uppercase;letter-spacing:.6px;color:#8b949e;margin:0 0 10px}
.stats{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.stat{background:#0d1117;border:1px solid #21262d;border-radius:8px;padding:10px;text-align:center}
.stat .v{font-size:26px;font-weight:700;color:#58a6ff;font-variant-numeric:tabular-nums}
.stat .l{font-size:11px;color:#8b949e;margin-top:2px}
.det{display:flex;align-items:center;gap:8px;padding:6px 0;border-bottom:1px solid #21262d;font-size:13px}
.det:last-child{border:none}
.dot{width:8px;height:8px;border-radius:50%;background:#f0a500;flex:0 0 auto}
.det .nm{flex:1;text-transform:capitalize}
.det .conf{color:#3fb950;font-weight:600;font-variant-numeric:tabular-nums}
.bar-bg{height:5px;background:#21262d;border-radius:3px;overflow:hidden;margin-top:3px}
.bar{height:100%;background:linear-gradient(90deg,#f0a500,#3fb950)}
.video-wrap{background:#010409;display:flex;align-items:center;justify-content:center;min-height:300px;position:relative}
#frame{max-width:100%;max-height:calc(100vh - 53px);object-fit:contain}
.btn{width:100%;padding:9px;border:none;border-radius:8px;background:#1f6feb;color:#fff;font-size:13px;font-weight:600;cursor:pointer;margin-top:6px}
.btn:hover{background:#388bfd}
.btn.sec{background:#21262d}.btn.sec:hover{background:#30363d}
.hint{font-size:12px;color:#8b949e;line-height:1.5}
.alert-on{outline:3px solid #f85149;outline-offset:-3px}
label.row{display:flex;align-items:center;gap:8px;font-size:13px;margin-top:6px}
select{background:#0d1117;color:#e6edf3;border:1px solid #30363d;border-radius:6px;padding:5px 8px;flex:1}
.tally{display:flex;flex-wrap:wrap;gap:6px;margin-top:6px}
.chip{background:#0d1117;border:1px solid #30363d;border-radius:14px;padding:3px 10px;font-size:12px;text-transform:capitalize}
.chip b{color:#58a6ff}
</style>
</head>
<body>
<header>
  <span class="logo">Y</span>
  <h1>PYNQ-Z2 · Tiny YOLO · DPU</h1>
  <span class="badge" id="live">CANLI</span>
  <span class="hint" id="status"></span>
</header>
<div class="wrap">
  <div class="panel">
    <div class="card">
      <h2>Performans</h2>
      <div class="stats">
        <div class="stat"><div class="v" id="fps">-</div><div class="l">FPS</div></div>
        <div class="stat"><div class="v" id="count">0</div><div class="l">Anlik nesne</div></div>
        <div class="stat"><div class="v" id="total">0</div><div class="l">Toplam tespit</div></div>
        <div class="stat"><div class="v" id="classes">0</div><div class="l">Farkli sinif</div></div>
      </div>
    </div>
    <div class="card">
      <h2>Anlik Tespitler</h2>
      <div id="dets"></div>
      <div class="hint" id="nodet">Nesne bekleniyor... Kamerayi bir nesneye dogrultun.</div>
    </div>
    <div class="card">
      <h2>Gorulen Siniflar</h2>
      <div class="tally" id="tally"><span class="hint">-</span></div>
    </div>
    <div class="card">
      <h2>Kontroller</h2>
      <button class="btn" id="snap">Snapshot indir (PNG)</button>
      <button class="btn sec" id="pause">Duraklat</button>
      <label class="row">Uyari sinifi:
        <select id="alertSel"><option value="">(kapali)</option></select>
      </label>
      <div class="hint" id="alertMsg"></div>
    </div>
  </div>
  <div class="video-wrap" id="vwrap">
    <img id="frame" alt="YOLO kamera" src="/frame">
  </div>
</div>
<script>
const COCO=["person","bicycle","car","motorcycle","airplane","bus","train","truck","boat","traffic light","fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse","sheep","cow","elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee","skis","snowboard","sports ball","kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket","bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple","sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair","couch","potted plant","bed","dining table","toilet","tv","laptop","mouse","remote","keyboard","cell phone","microwave","oven","toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear","hair drier","toothbrush"];
const sel=document.getElementById('alertSel');
COCO.forEach(c=>{const o=document.createElement('option');o.value=c;o.textContent=c;sel.appendChild(o);});
let paused=false, total=0; const seen={};
document.getElementById('pause').onclick=function(){paused=!paused;this.textContent=paused?'Devam et':'Duraklat';};
document.getElementById('snap').onclick=()=>{const a=document.createElement('a');a.href='/frame?t='+Date.now();a.download='yolo_'+Date.now()+'.png';a.click();};
async function tick(){
  if(!paused){
    try{
      const r=await fetch('/data',{cache:'no-store'});
      const d=await r.json();
      document.getElementById('fps').textContent=d.fps>0?d.fps.toFixed(1):'-';
      const dets=d.detections||[];
      document.getElementById('count').textContent=dets.length;
      const box=document.getElementById('dets');const nodet=document.getElementById('nodet');
      box.innerHTML='';
      if(dets.length){
        nodet.style.display='none';
        dets.forEach(x=>{
          total++; seen[x.label]=(seen[x.label]||0)+1;
          const row=document.createElement('div');row.className='det';
          row.innerHTML='<span class="dot"></span><div style="flex:1"><div style="display:flex;justify-content:space-between"><span class="nm">'+x.label+'</span><span class="conf">'+(x.conf*100).toFixed(0)+'%</span></div><div class="bar-bg"><div class="bar" style="width:'+(x.conf*100)+'%"></div></div></div>';
          box.appendChild(row);
        });
      }else nodet.style.display='block';
      document.getElementById('total').textContent=total;
      const ks=Object.keys(seen);
      document.getElementById('classes').textContent=ks.length;
      const tally=document.getElementById('tally');
      tally.innerHTML=ks.length?'':'<span class="hint">-</span>';
      ks.sort((a,b)=>seen[b]-seen[a]).forEach(k=>{const c=document.createElement('span');c.className='chip';c.innerHTML=k+' <b>'+seen[k]+'</b>';tally.appendChild(c);});
      const al=sel.value;const vw=document.getElementById('vwrap');const am=document.getElementById('alertMsg');
      if(al&&dets.some(x=>x.label===al)){vw.classList.add('alert-on');am.textContent='UYARI: "'+al+'" tespit edildi!';am.style.color='#f85149';}
      else{vw.classList.remove('alert-on');am.textContent=al?('"'+al+'" izleniyor...'):'';am.style.color='#8b949e';}
      const ago=d.seconds_ago!=null?d.seconds_ago:999;
      document.getElementById('status').textContent=ago<2?'canli':'son kare '+Math.round(ago)+' sn once';
      document.getElementById('live').style.background=ago<3?'#1a7f37':'#9e6a03';
    }catch(e){}
    document.getElementById('frame').src='/frame?t='+Date.now();
  }
}
setInterval(tick,350);
tick();
</script>
</body>
</html>)HTML";

static string build_json() {
    vector<WebDet> dets;
    float fps;
    double last_mono;
    {
        lock_guard<mutex> lk(g_mtx);
        dets = g_dets;
        fps = g_fps.load();
        last_mono = g_last_frame_mono.load();
    }
    double now = chrono::steady_clock::now().time_since_epoch().count() / 1e9;
    double ago = (last_mono > 0) ? (now - last_mono) : 9999.0;

    ostringstream os;
    os << "{\"fps\":" << fps << ",\"seconds_ago\":" << ago << ",\"detections\":[";
    for (size_t i = 0; i < dets.size(); ++i) {
        if (i) os << ',';
        os << "{\"label\":\"" << json_escape(dets[i].label) << "\""
           << ",\"conf\":" << dets[i].conf
           << ",\"xmin\":" << dets[i].xmin << ",\"ymin\":" << dets[i].ymin
           << ",\"xmax\":" << dets[i].xmax << ",\"ymax\":" << dets[i].ymax << "}";
    }
    os << "]}";
    return os.str();
}

static void send_all(int fd, const string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }
}

static void handle_client(int c) {
    char buf[1024];
    ssize_t n = recv(c, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(c); return; }
    buf[n] = 0;
    string req(buf);
    string path = "/";
    size_t sp = req.find(' ');
    if (sp != string::npos) {
        size_t sp2 = req.find(' ', sp + 1);
        if (sp2 != string::npos) path = req.substr(sp + 1, sp2 - sp - 1);
    }

    if (path.find("/frame") == 0) {
        vector<uchar> jpeg;
        { lock_guard<mutex> lk(g_mtx); jpeg = g_jpeg; }
        if (jpeg.empty()) {
            string hdr = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
            send_all(c, hdr);
        } else {
            ostringstream hdr;
            hdr << "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
                << "Content-Length: " << jpeg.size()
                << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
            send_all(c, hdr.str());
            send(c, (const char*)jpeg.data(), jpeg.size(), 0);
        }
    } else if (path.find("/data") == 0) {
        string body = build_json();
        ostringstream hdr;
        hdr << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            << "Content-Length: " << body.size()
            << "\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n"
            << body;
        send_all(c, hdr.str());
    } else {
        string body(HTML_PAGE);
        ostringstream hdr;
        hdr << "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
            << "Content-Length: " << body.size()
            << "\r\nConnection: close\r\n\r\n"
            << body;
        send_all(c, hdr.str());
    }
    close(c);
}

static void http_server_thread() {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return;
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(WEB_PORT);
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(srv);
        return;
    }
    listen(srv, 4);
    cout << "Web pano: http://192.168.2.99:" << WEB_PORT << " (veya kart IP)\n";
    while (g_running) {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int c = accept(srv, (sockaddr*)&cli, &len);
        if (c < 0) continue;
        handle_client(c);
    }
    close(srv);
}

int main() {
    thread http_thr(http_server_thread);
    http_thr.detach();

    dpuOpen();
    DPUKernel* kernel = dpuLoadKernel(YOLOKERNEL);
    DPUTask* task = dpuCreateTask(kernel, 0);
    int sh = dpuGetInputTensorHeight(task, INPUTNODE);
    int sw = dpuGetInputTensorWidth(task, INPUTNODE);

    VideoCapture cap(0);
    if (!cap.isOpened()) {
        cerr << "Kamera acilamadi (/dev/video0)\n";
        return 1;
    }
    cap.set(CAP_PROP_FOURCC, VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(CAP_PROP_FRAME_WIDTH, 640);
    cap.set(CAP_PROP_FRAME_HEIGHT, 480);
    cap.set(CAP_PROP_BUFFERSIZE, 1);

    auto t_prev = chrono::steady_clock::now();
    size_t frames = 0;
    Mat img;
    while (g_running && cap.read(img) && !img.empty()) {
        set_input_image_fast(task, img, INPUTNODE);
        dpuRunTask(task);
        deal_web(task, img, sw, sh);
        frames++;

        auto now = chrono::steady_clock::now();
        double secs = chrono::duration<double>(now - t_prev).count();
        if (secs >= 1.0) {
            g_fps.store((float)(frames / secs));
            frames = 0;
            t_prev = now;
        }
    }

    g_running = false;
    cap.release();
    dpuDestroyTask(task);
    dpuDestroyKernel(kernel);
    dpuClose();
    return 0;
}
