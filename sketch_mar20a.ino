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

// Configurações do NTP
const char* ntpServer = "pool.ntp.org";  // Servidor NTP
const long gmtOffset_sec = -3 * 3600;    // Fuso horário (em segundos) para GMT-3 (Brasília)
const int daylightOffset_sec = 0;        // Offset para horário de verão (0 se não estiver em vigor)

// Tamanho da tela do display
#define DISP_WIDTH  320
#define DISP_HEIGHT 480

#define LV_COLOR_GREEN lv_color_hex(0x4CAF50)   // Verde
#define LV_COLOR_RED   lv_color_make(255, 0, 0)   // Vermelho

unsigned long lastActivityTime = 0;  // Guarda o tempo da última interação
const unsigned long screenTimeout = 5 * 60 * 1000; // 5 minutos em milissegundos
bool screenOff = false;  // Estado da tela ligada/desligada

lv_obj_t * temp_label;
lv_obj_t * hum_label;

void turnOffScreen() {
    if (!screenOff) {
        Serial.println("Desligando tela por inatividade...");
        bsp_display_backlight_off(); // Desliga o backlight do display
        screenOff = true;
    }
}

void turnOnScreen() {
    if (screenOff) {
        Serial.println("Ligando tela...");
        bsp_display_backlight_on(); // Liga o backlight do display
        screenOff = false;
        lastActivityTime = millis(); // Reseta o contador de inatividade
    }
}

// Função para criar botões
void addButton(const char * name, int x, int y, int w, int h, int button_id) {
    lv_obj_t * scr = lv_scr_act();  // Obtém a tela ativa
    
    // Criar o botão
    lv_obj_t * btn = lv_btn_create(scr);
    lv_obj_set_pos(btn, x, y);  // Define a posição do botão
    lv_obj_set_size(btn, w, h);  // Define o tamanho do botão
    
    // Atribuir o ID ao botão para que você possa identificar
    lv_obj_set_user_data(btn, (void *)button_id);

    // Criar um rótulo (label) dentro do botão
    lv_obj_t * label = lv_label_create(btn);
    lv_label_set_text(label, name);  // Define o texto do botão
    
    // Definir o estilo do botão (se necessário)
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);  // Exemplo de borda arredondada
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x4CAF50), LV_PART_MAIN);  // Exemplo de cor de fundo verde
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);  // Exemplo de largura da borda
    
    // Adicionar um evento de clique ao botão
    lv_obj_add_event_cb(btn, btn_event_handler, LV_EVENT_CLICKED, NULL);  // Define a função de callback
}

lv_obj_t* findButtonById(int button_id) {
    lv_obj_t * scr = lv_scr_act();
    lv_obj_t * btn = lv_obj_get_child(scr, NULL);  // Começa pelo primeiro filho

    while (btn != NULL) {
        int btn_id = (int)lv_obj_get_user_data(btn);  // Obtém o ID do botão
        if (btn_id == button_id) {
            return btn;  // Retorna o botão com o ID correspondente
        }
        btn = lv_obj_get_child(btn, NULL);  // Pega o próximo filho
    }

    return NULL;  // Retorna NULL caso não encontre o botão
}

// Função para alterar a cor do botão com base no ID
void changeButtonColorById(int button_id, lv_color_t color) {
    lv_obj_t * scr = lv_scr_act();  // Obtém a tela ativa
    lv_obj_t * btn = NULL;
    
    // Itera sobre todos os objetos na tela ativa
    lv_obj_t * obj = lv_obj_get_child(scr, NULL);  // Obtém o primeiro filho da tela
    int index = 0;  // Índice para avançar pelos filhos

    // Percorre todos os objetos filhos até o final
    while (obj != NULL) {
        // Verifica se o objeto é um botão e se o ID corresponde
        if (lv_obj_get_user_data(obj) == (void*)button_id) {
            btn = obj;  // Encontra o botão com o ID correspondente
            break;
        }
        index++;  // Incrementa o índice para avançar
        obj = lv_obj_get_child(scr, index);  // Avança para o próximo filho
    }

    // Se o botão foi encontrado, altera sua cor de fundo
    if (btn != NULL) {
        lv_obj_set_style_bg_color(btn, color, LV_PART_MAIN);  // Muda a cor do fundo
    } else {
        Serial.println("Botão com ID não encontrado");
    }
}

void criar_popup(const char *mensagem) {
    static lv_obj_t *popup = NULL;
    
    if (popup != NULL) {
        lv_obj_del(popup);  // Remove o popup anterior se já existir
    }

    popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(popup, 250, 120);
    lv_obj_align(popup, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *label = lv_label_create(popup);
    lv_label_set_text(label, mensagem);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *btn = lv_btn_create(popup);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(btn, [](lv_event_t * e) {
        lv_obj_del(popup);  // Remove o popup ao clicar no botão OK
        popup = NULL;
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "OK");
}

// Configurações iniciais
WebServer server(80);
Preferences preferences;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Variáveis para armazenar as configurações
String ssid, password, mqtt_host, mqtt_user, mqtt_pass, mqtt_topic;
int mqtt_port;

// Variável para o label do relógio
lv_obj_t *clock_label;
unsigned long lastUpdate = 0;

void update_clock() {
  struct tm timeinfo;  // Estrutura para armazenar o horário
  if (!getLocalTime(&timeinfo)) {  // Obtém o horário atual
      Serial.println("Falha ao obter o horário");
      return;
  }

  // Formata o horário como "HH:MM" (sem segundos)
  char time_str[20];
  strftime(time_str, sizeof(time_str), "%H:%M", &timeinfo);  // Alterado para "%H:%M"

  // Atualiza o texto do label do relógio
  lv_label_set_text(clock_label, time_str);
}

void saveWiFiConfig() {
    preferences.begin("config", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.end();
}

void saveMQTTConfig() {
    preferences.begin("config", false);
    preferences.putString("mqtt_host", mqtt_host);
    preferences.putInt("mqtt_port", mqtt_port);
    preferences.putString("mqtt_user", mqtt_user);
    preferences.putString("mqtt_pass", mqtt_pass);
    preferences.putString("mqtt_topic", mqtt_topic);
    preferences.end();
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
            mqttClient.subscribe("stat/tasmota_DF83E9/POWER");
            mqttClient.subscribe("tele/tasmota_DF83E9/SENSOR");
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

static void btn_event_handler(lv_event_t * e) {
  lv_obj_t * btn = lv_event_get_target(e);  // Obtém o objeto que gerou o evento
  int button_id = (int)lv_obj_get_user_data(btn);
  Serial.print("Botão pressionado com ID: ");
  Serial.println(button_id);

  lv_obj_t * popup = (lv_obj_t *)lv_event_get_user_data(e);  // Obtém o popup do evento

  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    const char *btn_label = lv_label_get_text(lv_obj_get_child(btn, NULL));  // Obtém o texto do botão
    
    if (strcmp(btn_label, "OK") == 0) {
      // Fechar o popup quando o botão "OK" for pressionado
      lv_obj_del(popup);  // Remove o popup da tela
    }

    if (strcmp(btn_label, "Ventilador") == 0) {
      mqttClient.publish("cmnd/tasmota_DF83E9/POWER", "TOGGLE");
    }
    
    else {
      // Enviar uma mensagem MQTT com o nome do botão
      String mensagem = String(btn_label);  // Converte o rótulo do botão para String

      // Remove o curinga do tópico e adiciona o nome do botão
      String topicoPublicacao = mqtt_topic;
      topicoPublicacao.replace("/#", "");  // Remove o curinga
      //topicoPublicacao += "/" + String(btn_label);  // Adiciona o nome do botão ao tópico

      // Publica a mensagem no tópico MQTT
      mqttClient.publish(topicoPublicacao.c_str(), mensagem.c_str());

      // Log no Serial Monitor
      Serial.println("Botão Pressionado: " + mensagem);
      Serial.println("Mensagem MQTT enviada no tópico: " + topicoPublicacao);
    }
  }
}

// Função de callback para mensagens MQTT
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Mensagem recebida no tópico: ");
  Serial.println(topic);
  Serial.print("Mensagem: ");
  
  // Converte o payload (mensagem) para uma string
  String mensagem;
  for (unsigned int i = 0; i < length; i++) {
      mensagem += (char)payload[i];
  }
  Serial.println(mensagem);

  // Converte o tópico recebido para um objeto String
  String topicoRecebido = String(topic);

  // Remove o curinga "#" do mqtt_topic para comparação
  String topicoBase = mqtt_topic;
  topicoBase.replace("/#", "");  // Remove o curinga, se presente

  // Verifica se o tópico recebido é igual a "topicoBase/popup"
  if (topicoRecebido == topicoBase + "/popup") {
      // Cria um popup com a mensagem recebida
      criar_popup(mensagem.c_str());
  }

  // Verifica se a mensagem é sobre o status do POWER e se for "ON"
  if (topicoRecebido == String("stat/tasmota_DF83E9/POWER")) {
      if (mensagem == "ON") {
          // Altera a cor do botão com ID 7 para verde
          changeButtonColorById(7, LV_COLOR_RED);
      } else if (mensagem == "OFF") {
          // Altera a cor do botão com ID 7 para a cor padrão (vermelho)
          changeButtonColorById(7, LV_COLOR_GREEN);
      }
  }

  // Verifica se é mensagem do sensor
  if (String(topic) == "tele/tasmota_DF83E9/SENSOR") {
    // Procura os valores no JSON
    int dhtStart = mensagem.indexOf("\"DHT11\":{");
    if (dhtStart < 0) {
      Serial.println("Dados DHT11 não encontrados na mensagem");
      return;
    }

    int tempStart = mensagem.indexOf("\"Temperature\":", dhtStart) + 14;
    int tempEnd = mensagem.indexOf(",", tempStart);
    int humStart = mensagem.indexOf("\"Humidity\":", dhtStart) + 11;
    int humEnd = mensagem.indexOf(",", humStart);

    if (tempStart > 0 && tempEnd > 0 && humStart > 0 && humEnd > 0) {
      String tempStr = mensagem.substring(tempStart, tempEnd);
      String humStr = mensagem.substring(humStart, humEnd);
      
      float temperatura = tempStr.toFloat();
      float umidade = humStr.toFloat();

      // Atualiza os labels na tela
      char tempText[20];
      char humText[20];
      snprintf(tempText, sizeof(tempText), "Temp: %.1f°C", temperatura);
      snprintf(humText, sizeof(humText), "Hum: %.1f%%", umidade);
      
      lv_label_set_text(temp_label, tempText);
      lv_label_set_text(hum_label, humText);

      Serial.printf("Temperatura: %.1f°C, Umidade: %.1f%%\n", temperatura, umidade);
    } else {
      Serial.println("Não foi possível extrair temperatura/umidade da mensagem");
    }
  }
}

static void touch_event_handler(lv_event_t * e) {
    Serial.println("Tela tocada!");
    turnOnScreen();
}

void setup() {
  Serial.begin(115200);
  
  esp_task_wdt_config_t wdt_config = {
      .timeout_ms = 10000, // 10 segundos
      .idle_core_mask = (1 << 0), // Aplicado ao core 0
      .trigger_panic = true
  };
  
  esp_task_wdt_init(&wdt_config);  // Passa a estrutura corretamente
  esp_task_wdt_add(NULL);    // Adiciona a tarefa principal ao watchdog

  lv_init();
  // Inicializar o display usando a configuração do BSP
  bsp_display_cfg_t cfg = {
      .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
      .buffer_size = DISP_WIDTH * DISP_HEIGHT, 
      .rotate = LV_DISP_ROT_NONE,  
  };

  bsp_display_start_with_config(&cfg);
  bsp_display_backlight_on();
  //bsp_display_brightness_set(150);

  // Criar a tela
  lv_obj_t * scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);

  // Criar a label acima do botão
  lv_obj_t * label_top = lv_label_create(scr);
  lv_label_set_text(label_top, "Caos House");
  lv_obj_align(label_top, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_set_style_text_color(label_top, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer); 
  // Criar o label do relógio
  clock_label = lv_label_create(scr);  // Cria o label na tela atual
  lv_label_set_text(clock_label, "00:00:00");  // Define o texto inicial
  lv_obj_align(clock_label, LV_ALIGN_TOP_RIGHT, -20, 10);  // Alinha no canto superior direito
  lv_obj_set_style_text_color(clock_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);  // Cor do texto (branco)
  lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_16, LV_PART_MAIN);  // Fonte do texto

    // Criar o label da temperatura (abaixo do relógio)
  temp_label = lv_label_create(scr);  // Cria o label da temperatura
  lv_label_set_text(temp_label, "Temp: --.---");  // Texto inicial (sem dados)
  lv_obj_align(temp_label, LV_ALIGN_TOP_RIGHT, -20, 40);  // Alinha abaixo do relógio (com 30px de distância)
  lv_obj_set_style_text_color(temp_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);  // Cor do texto (branco)
  lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_16, LV_PART_MAIN);  // Fonte do texto

  // Criar o label da umidade (abaixo da temperatura)
  hum_label = lv_label_create(scr);  // Cria o label da umidade
  lv_label_set_text(hum_label, "Hum: --.-");  // Texto inicial (sem dados)
  lv_obj_align(hum_label, LV_ALIGN_TOP_RIGHT, -20, 60);  // Alinha abaixo da temperatura (com 20px de distância)
  lv_obj_set_style_text_color(hum_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);  // Cor do texto (branco)
  lv_obj_set_style_text_font(hum_label, &lv_font_montserrat_16, LV_PART_MAIN);  // Fonte do texto

  // Criando os botões
  addButton("Luz da Sala",         5,  400, 300, 60, 1);
  addButton("Luz da sala",         5,  535, 140, 100, 1);
  addButton("Luz do \nescritorio", 160, 535, 140, 100, 2);
  addButton("Luz da cozinha",      5,  650, 140, 100, 3);
  addButton("Luz da copa",         160, 650, 140, 100, 4);
  addButton("Luz do PC",           5,  770, 140, 100, 5);
  addButton("Luz do quarto",       160, 770, 140, 100, 6);
  addButton("Ventilador",          5,  900, 300, 60, 7);
  
  // Criar um popup
  criar_popup("jogar lixo fora");

  // Ajustar o brilho (0 a 255, sendo 0 totalmente apagado e 255 totalmente brilhante)
  ledcWrite(0, 128);  // Brilho em 50%

  loadConfig();
  connectWiFi();
  
  server.on("/", handleRoot);
  server.on("/wifi", HTTP_POST, handleWiFiConfig);
  server.on("/mqtt", HTTP_POST, handleMQTTConfig);
  server.on("/send", HTTP_POST, handleSend);
  server.begin();

  // Configura o callback para o MQTT
  mqttClient.setCallback(callback);

  lv_obj_add_event_cb(lv_scr_act(), touch_event_handler, LV_EVENT_PRESSED, NULL);
  
  lastActivityTime = millis(); // Inicializa o contador de atividade
}

void loop() {
  esp_task_wdt_reset();
  server.handleClient();

  // Verifica a conexão MQTT e processa as mensagens recebidas
  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();

    // Atualiza o relógio a cada segundo
  if (millis() - lastUpdate >= 1000) {  // 1000 ms = 1 segundo
    update_clock();
    lastUpdate = millis();  // Atualiza o tempo da última chamada
  }

  lv_task_handler();
  // Verifica se passou 5 minutos sem atividade
  if (millis() - lastActivityTime > screenTimeout) {
      turnOffScreen();
  }
  delay(5);
}
