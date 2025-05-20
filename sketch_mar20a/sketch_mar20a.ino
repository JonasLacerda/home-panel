#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <lvgl.h>
#include "display.h"
#include "esp_bsp.h"
#include "lv_port.h"
#include <time.h>
#include "esp_task_wdt.h"
#include <ArduinoJson.h>
#include <set> 
#include <map>
#include <string>

std::map<std::string, lv_obj_t*> sensor_labels;
lv_coord_t sensor_y_offset = 150; // Posição Y inicial para os sensores

std::vector<String> createdSwitches;

//nao esquecer da tela de popup!!!

// Configurações do display
#define DISP_WIDTH  320
#define DISP_HEIGHT 480

// Configurações do NTP
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600;
const int daylightOffset_sec = 0;

// Variáveis globais
WebServer server(80);
Preferences preferences;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Configurações WiFi/MQTT
String ssid, password, mqtt_host, mqtt_user, mqtt_pass, mqtt_topic;
int mqtt_port;

// Variáveis para controle da tela
unsigned long lastActivityTime = 0;
const unsigned long screenTimeout = 5 * 60 * 1000; // 5 minutos
bool screenOff = false;

// Variáveis LVGL
lv_obj_t *main_screen;
lv_obj_t *switches_screen;
lv_obj_t *switches_container;
lv_obj_t *sensor_container;
bool current_screen = false; // false=main, true=switches

// Elementos da interface
lv_obj_t *clock_label;
lv_obj_t *date_label;
lv_obj_t *temp_label;
lv_obj_t *hum_label;

// Implementação das funções de configuração
void saveWiFiConfig() {
  preferences.begin("config", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();
  Serial.println("Configurações WiFi salvas");
}

void saveMQTTConfig() {
  preferences.begin("config", false);
  preferences.putString("mqtt_host", mqtt_host);
  preferences.putInt("mqtt_port", mqtt_port);
  preferences.putString("mqtt_user", mqtt_user);
  preferences.putString("mqtt_pass", mqtt_pass);
  preferences.putString("mqtt_topic", mqtt_topic);
  preferences.end();
  Serial.println("Configurações MQTT salvas");
}

void loadConfig() {
  preferences.begin("config", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  mqtt_host = preferences.getString("mqtt_host", "");
  mqtt_port = preferences.getInt("mqtt_port", 1883);
  mqtt_user = preferences.getString("mqtt_user", "");
  mqtt_pass = preferences.getString("mqtt_pass", "");
  mqtt_topic = preferences.getString("mqtt_topic", "controle/#");
  preferences.end();
  Serial.println("Configurações carregadas");
}

// Protótipos das outras funções
void create_main_screen();
void create_switches_screen();
void update_clock();
void update_date();
void turnOffScreen();
void turnOnScreen();
void connectWiFi();
void connectMQTT();
void handleRoot();
void handleWiFiConfig();
void handleMQTTConfig();
void handleSend();
void callback(char* topic, byte* payload, unsigned int length);
void addButton(const char *name, lv_obj_t *parent);
void switch_screen(lv_event_t *e);
void turnAllOn(lv_event_t *e);
void turnAllOff(lv_event_t *e);

void setup() {
  Serial.begin(115200);
  
  // Configura Watchdog
  esp_task_wdt_config_t wdt_config = {
      .timeout_ms = 10000,
      .idle_core_mask = (1 << 0),
      .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  // Inicializa LVGL e display
  lv_init();
  bsp_display_cfg_t cfg = {
      .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
      .buffer_size = DISP_WIDTH * DISP_HEIGHT, 
      .rotate = LV_DISP_ROT_NONE,  
  };
  bsp_display_start_with_config(&cfg);
  bsp_display_backlight_on();

  // Cria as telas
  main_screen = lv_obj_create(NULL);
  switches_screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(main_screen, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_color(switches_screen, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_scr_load(main_screen);

  // Configura NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // Cria as interfaces
  create_main_screen();
  create_switches_screen();

  // Configura WiFi e MQTT
  loadConfig();
  connectWiFi();
  
  server.on("/", handleRoot);
  server.on("/wifi", HTTP_POST, handleWiFiConfig);
  server.on("/mqtt", HTTP_POST, handleMQTTConfig);
  server.on("/send", HTTP_POST, handleSend);
  server.begin();

  mqttClient.setCallback(callback);
  connectMQTT();

  mqttClient.publish("node/cmd", "interruptor");
  Serial.println("Solicitada lista de interruptores...");

  lastActivityTime = millis();
}

void loop() {
  esp_task_wdt_reset();
  server.handleClient();

  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();

  // Atualiza relógio a cada segundo
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate >= 1000) {
    update_clock();
    update_date();
    lastUpdate = millis();
  }

  // Verifica inatividade - com verificação adicional
  if (millis() - lastActivityTime > screenTimeout && !screenOff) {
    // Verificação extra para garantir que está na tela principal
    if (lv_scr_act() != main_screen) {
      current_screen = false;
      lv_scr_load(main_screen);
      lv_task_handler();
      delay(50);
    }
    turnOffScreen();
  }

  lv_task_handler();
  delay(5);
}

void create_main_screen() {
  // Cria a tela
  main_screen = lv_obj_create(NULL);
  lv_scr_load(main_screen);

  // Define a cor de fundo como PRETA
  lv_obj_set_style_bg_color(main_screen, lv_color_hex(0x000000), LV_PART_MAIN);

  // Título
  lv_obj_t *title = lv_label_create(main_screen);
  lv_label_set_text(title, "Caos House");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);

  // Relógio
  clock_label = lv_label_create(main_screen);
  lv_label_set_text(clock_label, "00:00");
  lv_obj_align(clock_label, LV_ALIGN_TOP_RIGHT, -20, 20);
  lv_obj_set_style_text_color(clock_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

  // Data
  date_label = lv_label_create(main_screen);
  lv_label_set_text(date_label, "01/01/2023");
  lv_obj_align(date_label, LV_ALIGN_TOP_RIGHT, -20, 50);
  lv_obj_set_style_text_color(date_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

  // 🔽 Cria container com rolagem para sensores
  sensor_container = lv_obj_create(main_screen);
  lv_obj_set_size(sensor_container, 300, 150);
  lv_obj_align(sensor_container, LV_ALIGN_TOP_LEFT, 10, 90);
  lv_obj_set_scroll_dir(sensor_container, LV_DIR_VER);
  lv_obj_set_style_bg_opa(sensor_container, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_style_border_width(sensor_container, 0, LV_PART_MAIN);
  lv_obj_add_flag(sensor_container, LV_OBJ_FLAG_SCROLLABLE);

  // Layout flex em coluna
  lv_obj_set_layout(sensor_container, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(sensor_container, LV_FLEX_FLOW_COLUMN);


  // Botão interruptores
  lv_obj_t *btn = lv_btn_create(main_screen);
  lv_obj_set_size(btn, 200, 50);
  lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_add_event_cb(btn, switch_screen, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *btn_label = lv_label_create(btn);
  lv_label_set_text(btn_label, "Interruptores");
}

void create_switches_screen() {
  // Container com rolagem
  switches_container = lv_obj_create(switches_screen);
  lv_obj_set_size(switches_container, DISP_WIDTH - 20, DISP_HEIGHT - 110);
  lv_obj_align(switches_container, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_set_flex_flow(switches_container, LV_FLEX_FLOW_COLUMN_WRAP);
  lv_obj_set_style_pad_all(switches_container, 10, LV_PART_MAIN);
  lv_obj_set_style_border_width(switches_container, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(switches_container, lv_color_hex(0x000000), LV_PART_MAIN);
  
  // Configuração de rolagem
  lv_obj_set_scrollbar_mode(switches_container, LV_SCROLLBAR_MODE_AUTO);

  // Cria um container para os botões de controle
  lv_obj_t *control_btns = lv_obj_create(switches_screen);
  lv_obj_set_size(control_btns, DISP_WIDTH - 20, 50);
  lv_obj_align(control_btns, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_style_border_width(control_btns, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(control_btns, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_flex_flow(control_btns, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(control_btns, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  
  // Botão para ligar todos
  lv_obj_t *all_on_btn = lv_btn_create(control_btns);
  lv_obj_set_size(all_on_btn, 100, 40);
  lv_obj_set_style_bg_color(all_on_btn, lv_color_hex(0x4CAF50), LV_PART_MAIN);
  lv_obj_add_event_cb(all_on_btn, turnAllOn, LV_EVENT_CLICKED, NULL);
  lv_obj_t *all_on_label = lv_label_create(all_on_btn);
  lv_label_set_text(all_on_label, "Ligar");
  
  // Botão para desligar todos
  lv_obj_t *all_off_btn = lv_btn_create(control_btns);
  lv_obj_set_size(all_off_btn, 80, 40);
  lv_obj_set_style_bg_color(all_off_btn, lv_color_hex(0xF44336), LV_PART_MAIN);
  lv_obj_add_event_cb(all_off_btn, turnAllOff, LV_EVENT_CLICKED, NULL);
  lv_obj_t *all_off_label = lv_label_create(all_off_btn);
  lv_label_set_text(all_off_label, "Desligar");
  
  // Botão de voltar
  lv_obj_t *back_btn = lv_btn_create(control_btns);
  lv_obj_set_size(back_btn, 80, 40);
  lv_obj_add_event_cb(back_btn, switch_screen, LV_EVENT_CLICKED, NULL);
  lv_obj_t *back_label = lv_label_create(back_btn);
  lv_label_set_text(back_label, "Voltar");
}

void turnAllOn(lv_event_t *e) {
  turnOnScreen();

  uint32_t child_count = lv_obj_get_child_cnt(switches_container);

  for (uint32_t i = 0; i < child_count; i++) {
    lv_obj_t *btn = lv_obj_get_child(switches_container, i);

    lv_obj_set_style_bg_color(btn, lv_color_hex(0x4CAF50), LV_PART_MAIN);

    const char *entity = (const char *)lv_obj_get_user_data(btn);
    if (entity != NULL) {
      String command = String(entity) + " true";

      if (mqttClient.publish("node/cmd", command.c_str())) {
        Serial.printf("Comando enviado: %s\n", command.c_str());
      } else {
        Serial.printf("Falha ao enviar comando: %s\n", command.c_str());
      }

      delay(50);
      mqttClient.loop();
    }
  }
}

void turnAllOff(lv_event_t *e) {
  turnOffScreen();

  uint32_t child_count = lv_obj_get_child_cnt(switches_container);

  for (uint32_t i = 0; i < child_count; i++) {
    lv_obj_t *btn = lv_obj_get_child(switches_container, i);

    lv_obj_set_style_bg_color(btn, lv_color_hex(0x4A4A4A), LV_PART_MAIN);

    const char *entity = (const char *)lv_obj_get_user_data(btn);
    if (entity != NULL) {
      String command = String(entity) + " false";

      if (mqttClient.publish("node/cmd", command.c_str())) {
        Serial.printf("Comando enviado: %s\n", command.c_str());
      } else {
        Serial.printf("Falha ao enviar comando: %s\n", command.c_str());
      }

      delay(50);
      mqttClient.loop();
    }
  }
}

void switch_screen(lv_event_t *e) {
  turnOnScreen();
  
  // Se estamos indo para a tela de switches (não voltando)
  if (!current_screen) {
    // Envia o comando MQTT antes de trocar de tela
    if (mqttClient.publish("node/cmd", "interruptor")) {
      Serial.println("Solicitação de lista de interruptores enviada via MQTT");
    } else {
      Serial.println("Falha ao enviar solicitação de interruptores");
    }
    
    // Pequeno delay para garantir o envio antes da troca de tela
    delay(50);
    mqttClient.loop();
  }
  
  current_screen = !current_screen;
  lv_scr_load(current_screen ? switches_screen : main_screen);
  
  Serial.print("Tela carregada: ");
  Serial.println(current_screen ? "Switches" : "Principal");
  
  if(current_screen) {
    lv_obj_invalidate(switches_container);
    Serial.println("Container de switches invalidado para redesenho");
  }
}

// Atualiza relógio
void update_clock() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  char time_str[20];
  strftime(time_str, sizeof(time_str), "%H:%M", &timeinfo);
  lv_label_set_text(clock_label, time_str);
}

// Atualiza data
void update_date() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  char date_str[20];
  strftime(date_str, sizeof(date_str), "%d/%m/%Y", &timeinfo);
  lv_label_set_text(date_label, date_str);
}

// Funções para controle da tela
void turnOffScreen() {
  if (!screenOff) {
    Serial.println("Preparando para desligar tela...");
    
    // Força voltar para a tela principal independente do estado atual
    if (lv_scr_act() != main_screen) {
      current_screen = false;
      lv_scr_load(main_screen);
      lv_task_handler(); // Processa imediatamente a mudança
      delay(50); // Pequeno delay para garantir a transição
      Serial.println("Tela alterada para principal antes de desligar");
    }

    // Desliga o backlight
    bsp_display_backlight_off();
    screenOff = true;
    Serial.println("Backlight desligado");
  }
}

void turnOnScreen() {
  if (screenOff) {
    // Liga o backlight primeiro
    bsp_display_backlight_on();
    
    // Garante que estamos na tela principal
    if (lv_scr_act() != main_screen) {
      current_screen = false;
      lv_scr_load(main_screen);
      Serial.println("Restaurando tela principal ao ligar");
    }
    
    // Marca que a tela está ligada
    screenOff = false;

    // Atualiza o tempo de atividade (não inatividade) ao ligar a tela
    lastActivityTime = millis();
    Serial.println("Backlight ligado");

    // Força um refresh completo para garantir que a interface seja atualizada
    lv_refr_now(NULL);
  }
  
  lv_indev_t *indev = lv_indev_get_next(NULL);
  if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Se o ponto está dentro da tela, é provável que houve um toque recente
    if (point.x >= 0 && point.y >= 0) {
      lastActivityTime = millis();
    }
  }
}

void connectWiFi() {
  WiFi.begin(ssid.c_str(), password.c_str());
  Serial.print("Conectando ao WiFi");
  int attempt = 0;

  // Tentando se conectar ao Wi-Fi
  while (WiFi.status() != WL_CONNECTED && attempt < 20) {  // Tenta 20 vezes
    Serial.print(".");
    delay(500);
    attempt++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" Conectado ao WiFi!");
    Serial.print("IP Obtido: ");
    Serial.println(WiFi.localIP());  // Mostra o IP local obtido
    WiFi.softAPdisconnect(true);  // Desativa o modo AP se já estiver ativo
  } else {
    // Se não conseguiu conectar, cria o ponto de acesso
    Serial.println(" Falha ao conectar, criando ponto de acesso...");
    WiFi.softAP("ESP32_Config", "123456789"); // SSID e senha do AP
    Serial.print("Ponto de acesso criado. Conecte-se a rede: ESP32_Config\n");
    Serial.print("Endereço IP: ");
    Serial.println(WiFi.softAPIP()); // IP do ponto de acesso
  }
}

void connectMQTT() {
  mqttClient.setServer(mqtt_host.c_str(), mqtt_port);
  while (!mqttClient.connected()) {
    Serial.print("Conectando ao MQTT...");
    if (mqttClient.connect("ESP32", mqtt_user.c_str(), mqtt_pass.c_str())) {
      Serial.println(" Conectado!");
      mqttClient.subscribe(mqtt_topic.c_str());
      mqttClient.subscribe("node/status/interruptores");
    } else {
      Serial.print(" falhou, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" tentando novamente...");
      delay(5000);
    }
  }
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><title>Configuração ESP32</title>"
                  "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                  "<meta charset='UTF-8'>"
                  "<style>body { font-family: Arial, sans-serif; text-align: center; } form { max-width: 300px; margin: auto; }"
                  "input, button { width: 100%; margin: 5px 0; padding: 8px; }</style></head><body><h2>Configuração do ESP32</h2>"
                  "<form id='wifiForm'><h3>Wi-Fi</h3><input type='text' id='ssid' placeholder='SSID' value='" + ssid + "' required>"
                  "<input type='password' id='password' placeholder='Senha' value='" + password + "' required><button type='submit'>Salvar Wi-Fi</button></form>"
                  "<form id='mqttForm'><h3>MQTT</h3>"
                  "<input type='text' id='mqtt_host' placeholder='Host' value='" + mqtt_host + "' required>"
                  "<input type='number' id='mqtt_port' placeholder='Porta' value='" + String(mqtt_port) + "' required>"
                  "<input type='text' id='mqtt_user' placeholder='Usuário' value='" + mqtt_user + "'>"
                  "<input type='password' id='mqtt_pass' placeholder='Senha' value='" + mqtt_pass + "'>"
                  "<input type='text' id='mqtt_topic' placeholder='Tópico' value='" + mqtt_topic + "' required>"
                  "<button type='submit'>Salvar MQTT</button></form>"
                  "<h3>Enviar Mensagem MQTT</h3>"
                  "<input type='text' id='mqtt_message' placeholder='Mensagem' required>"
                  "<button onclick='sendMessage()'>Enviar</button>"
                  "<script>document.getElementById('wifiForm').addEventListener('submit', function(e) { e.preventDefault();"
                  "let data = new URLSearchParams(); data.append('ssid', document.getElementById('ssid').value);"
                  "data.append('password', document.getElementById('password').value);"
                  "fetch('/wifi', { method: 'POST', body: data }).then(response => response.text()).then(text => alert(text)); });"
                  "document.getElementById('mqttForm').addEventListener('submit', function(e) { e.preventDefault();"
                  "let data = new URLSearchParams(); data.append('mqtt_host', document.getElementById('mqtt_host').value);"
                  "data.append('mqtt_port', document.getElementById('mqtt_port').value); data.append('mqtt_user', document.getElementById('mqtt_user').value);"
                  "data.append('mqtt_pass', document.getElementById('mqtt_pass').value); data.append('mqtt_topic', document.getElementById('mqtt_topic').value);"
                  "fetch('/mqtt', { method: 'POST', body: data }).then(response => response.text()).then(text => alert(text)); });"
                  "function sendMessage() {"
                  "let message = document.getElementById('mqtt_message').value;"
                  "fetch('/send', { method: 'POST', body: new URLSearchParams({'message': message}) })"
                  ".then(response => response.text()).then(text => alert(text));"
                  "}</script></body></html>";

  server.send(200, "text/html", html);
}

void handleWiFiConfig() {
    if (server.hasArg("ssid")) ssid = server.arg("ssid");
    if (server.hasArg("password")) password = server.arg("password");

    saveWiFiConfig();
    connectWiFi();
    
    if (WiFi.status() == WL_CONNECTED) {
        server.send(200, "text/html", "<html><body><h1>Wi-Fi configurado e conectado!</h1></body></html>");
    } else {
        server.send(400, "text/html", "<html><body><h1>Erro: Não foi possível conectar ao Wi-Fi!</h1></body></html>");
    }
}

void handleMQTTConfig() {
  if (WiFi.status() == WL_CONNECTED) {
    if (server.hasArg("mqtt_host")) mqtt_host = server.arg("mqtt_host");
    if (server.hasArg("mqtt_port")) mqtt_port = server.arg("mqtt_port").toInt();
    if (server.hasArg("mqtt_user")) mqtt_user = server.arg("mqtt_user");
    if (server.hasArg("mqtt_pass")) mqtt_pass = server.arg("mqtt_pass");
    if (server.hasArg("mqtt_topic")) mqtt_topic = server.arg("mqtt_topic");

    saveMQTTConfig();
    connectMQTT();
      
    server.send(200, "text/html", "<html><body><h1>Configuração MQTT salva e conectado!</h1></body></html>");
  } else {
    server.send(400, "text/html", "<html><body><h1>Erro: O Wi-Fi não está conectado. Conecte-se ao Wi-Fi primeiro.</h1></body></html>");
  }
}

void handleSend() {
  if (server.hasArg("message")) {
    String message = server.arg("message");
    mqttClient.publish(mqtt_topic.c_str(), message.c_str());
    server.send(200, "text/html", "Mensagem enviada!");
  } else {
    server.send(400, "text/html", "Erro: Nenhuma mensagem recebida");
  }
}

static void btn_event_handler(lv_event_t *e) {
  lv_obj_t *btn = lv_event_get_target(e);
  const char *device_id = (const char*)lv_obj_get_user_data(btn); // pegar id

  if (device_id == NULL) {
    Serial.println("Erro: device_id é NULL");
    return;
  }
  
  if(strlen(device_id) == 0) {
    Serial.println("Erro: device_id vazio");
    return;
  }
  
  Serial.print("Device ID recebido: ");
  Serial.println(device_id);

  // converte para minúsculas
  String deviceIdLower = String(device_id);
  deviceIdLower.toLowerCase();
  
  lv_color_t current_color = lv_obj_get_style_bg_color(btn, LV_PART_MAIN);
  bool is_currently_on = (current_color.full == lv_color_hex(0x4CAF50).full);
  
  String full_command = deviceIdLower + " " + (is_currently_on ? "false" : "true");
  
  Serial.print("Comando a ser enviado: ");
  Serial.println(full_command);
  
  if(mqttClient.publish("node/cmd", full_command.c_str())) {
    Serial.println("Publicação MQTT bem-sucedida");
  } else {
    Serial.println("Falha na publicação MQTT");
  }
  
  // Atualiza a cor do botão (mesmo visual)
  lv_obj_set_style_bg_color(btn, 
  is_currently_on ? lv_color_hex(0x4A4A4A) : lv_color_hex(0x4CAF50),
  LV_PART_MAIN);
}

void callback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("Mensagem recebida [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  if (String(topic) == "node/status/interruptores") {
    processarListaInterruptores(message);
  }
}

void processarListaInterruptores(String jsonStr) {
  Serial.println("Processando lista de entidades...");
  Serial.println("JSON recebido: " + jsonStr);

  DynamicJsonDocument doc(4096); // Aumenta o buffer se o JSON for grande
  DeserializationError error = deserializeJson(doc, jsonStr);
  
  if (error) {
    Serial.print("Erro ao parsear JSON: ");
    Serial.println(error.c_str());
    return;
  }

  if (!doc.is<JsonArray>()) {
    Serial.println("Formato JSON inválido - esperado array de objetos");
    return;
  }

  JsonArray entidades = doc.as<JsonArray>();
  for (JsonObject entidade : entidades) {
    const char* id = entidade["id"];
    const char* name = entidade["name"];
    const char* state = entidade["state"];

    String entityId = String(id);
    String value = String(state);

    if (entityId.startsWith("switch.")) {
      // Trata como botão
      bool isOn = (value == "on");
      updateButtonState(id, isOn);

      if (std::find(createdSwitches.begin(), createdSwitches.end(), entityId) == createdSwitches.end()) {
        addButton(name, id, switches_container);
        createdSwitches.push_back(entityId);
      }

    } else if (entityId.startsWith("sensor.")) {
      // Trata como sensor
      update_sensor_label(entityId.c_str(), value.c_str());

    } else {
      Serial.println("Entidade ignorada: " + entityId);
    }
  }
}


void addButton(const char *name, const char *id, lv_obj_t *parent) {
  if (!name || !id || !parent) {
    Serial.println("Erro: parâmetros inválidos em addButton");
    return;
  }

  lv_obj_t *btn = lv_btn_create(parent);
  if (!btn) {
    Serial.println("Erro ao criar botão");
    return;
  }

  int btn_width = (DISP_WIDTH - 40) / 2;
  lv_obj_set_size(btn, btn_width, 100);
  lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn, 5, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x4A4A4A), LV_PART_MAIN);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *label = lv_label_create(btn);
  if (!label) {
    Serial.println("Erro ao criar label");
    lv_obj_del(btn);
    return;
  }

  // mostra o nome no label
  String text(name);
  text.replace("-", "-\n");

  lv_label_set_text(label, text.c_str());
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, btn_width - 10);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

  // guarda o id no user_data
  char *id_copy = (char*)malloc(strlen(id) + 1);
  if (!id_copy) {
    Serial.println("Erro ao alocar memória para id_copy");
    lv_obj_del(btn);
    return;
  }
  strcpy(id_copy, id);
  lv_obj_set_user_data(btn, (void*)id_copy);

  lv_obj_add_event_cb(btn, btn_event_handler, LV_EVENT_CLICKED, NULL);
}

void updateButtonState(const char *device_name, bool is_on) {
  uint32_t child_count = lv_obj_get_child_cnt(switches_container);
  
  for(uint32_t i = 0; i < child_count; i++) {
    lv_obj_t *btn = lv_obj_get_child(switches_container, i);
    const char *btn_id = (const char*)lv_obj_get_user_data(btn);
    
    if (btn_id != NULL && strcmp(btn_id, device_name) == 0) {
      lv_color_t color = is_on ? lv_color_hex(0x4CAF50) : lv_color_hex(0x4A4A4A);
      lv_obj_set_style_bg_color(btn, color, LV_PART_MAIN);
      break;
    }
  }
}

void update_sensor_label(const std::string& entity_id, const std::string& value) {
  lv_obj_t* label;

  std::string display_name = entity_id;
  const std::string prefix = "sensor.";
  if (display_name.rfind(prefix, 0) == 0) {
    display_name = display_name.substr(prefix.length());
  }

  if (sensor_labels.count(display_name)) {
    label = sensor_labels[display_name];
    std::string label_text = display_name + ": " + value;
    lv_label_set_text(label, label_text.c_str());
  } else {
    // Cria novo label no container com scroll
    label = lv_label_create(sensor_container);
    std::string label_text = display_name + ": " + value;
    lv_label_set_text(label, label_text.c_str());
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    sensor_labels[display_name] = label;
  }
}
