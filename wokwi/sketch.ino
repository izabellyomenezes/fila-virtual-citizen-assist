/*
 * Citizen Assist AI - Fila Virtual do Mutirão de Documentos (RS 2024)
 * Disciplina: Arquitetura e Organização de Computadores
 * Placa: ESP32 DevKit C V4 (simulada no Wokwi)
 *
 * O programa conta as pessoas na fila (botão verde = chegou alguém,
 * botão vermelho = atendimento concluído), calcula o tempo de espera
 * pela média e pela IA, mostra no LCD, escreve um log no Serial e
 * manda os dados por MQTT para o painel web.
 *
 * Arquitetura: o Xtensa LX6 do ESP32 usa Harvard modificada. Instruções
 * (IRAM) e dados (DRAM) ficam em memórias separadas, com barramentos
 * separados, e o código da Flash é lido através de uma cache.
 *
 *   Entrada: botões nos GPIO 18 e 19
 *   Processamento: CPU Xtensa LX6
 *   Memória: IRAM (instruções), DRAM (variáveis), cache e Flash
 *            (programa e fila salva)
 *   Saída: LCD (I2C), LED (GPIO 2), Serial e MQTT (Wi-Fi)
 */

#include <WiFi.h>
#include <PubSubClient.h>       // MQTT
#include <Wire.h>               // I2C do LCD
#include <LiquidCrystal_I2C.h>
#include <time.h>               // horário pelo NTP
#include <Preferences.h>        // salvar dados na Flash

// ----- Configurações -----

// Pinos (iguais aos do diagram.json)
const int PINO_BOTAO_VERDE    = 18;
const int PINO_BOTAO_VERMELHO = 19;
const int PINO_LED_ERRO       = 2;

// Wi-Fi do Wokwi (rede aberta)
const char* WIFI_NOME  = "Wokwi-GUEST";
const char* WIFI_SENHA = "";

const char* MQTT_BROKER = "broker.hivemq.com";
const int   MQTT_PORTA  = 1883;

// Tem que ser igual ao TOPICO do painel/index.html
const char* MQTT_TOPICO = "citizenassist/fila/gs2026-1ccfev-x7k3";

// Coeficientes da IA, tirados do notebook (dados simulados).
// espera = A x pessoas_na_fila + B x pessoas_no_pico + C
const float COEF_A = 2.4998;   // min por pessoa fora do pico
const float COEF_B = 1.0157;   // min a mais por pessoa no pico
const float COEF_C = -0.2486;

// Tempos em milissegundos
const unsigned long TEMPO_DEBOUNCE   = 200;
const unsigned long TEMPO_ERRO       = 1000;
const unsigned long INTERVALO_MQTT   = 10000;
const unsigned long INTERVALO_RECON  = 5000;   // MQTT
const unsigned long INTERVALO_WIFI   = 30000;
const unsigned long LIMITE_WIFI      = 30000;  // espera máxima no setup
const unsigned long TEMPO_ZERAR      = 3000;
const unsigned long INTERVALO_DESEMPENHO = 10000000;  // 10 s em microssegundos

const int MAX_TEMPOS  = 20;
const int MAX_EVENTOS = 20;

LiquidCrystal_I2C lcd(0x27, 16, 2);   // endereço 0x27, 16 colunas, 2 linhas
WiFiClient clienteWifi;
PubSubClient clienteMqtt(clienteWifi);
Preferences memoria_flash;

// ----- Variáveis globais (ficam na DRAM) -----

int pessoas_na_fila = 0;
int atendimentos_concluidos = 0;

// Vetor circular: quando enche, o valor novo entra no lugar do mais antigo
float tempos_atendimento[MAX_TEMPOS];   // em segundos
int quantidade_tempos = 0;
int posicao_tempo = 0;
unsigned long ultimo_atendimento_ms = 0;

struct Evento {
  char tipo[8];       // "ENTRADA", "SAIDA", "ERRO" ou "ZERAR"
  char horario[20];   // "AAAA-MM-DD HH:MM:SS"
  int fila;
};

Evento eventos[MAX_EVENTOS];   // log circular dos últimos 20 eventos
int quantidade_eventos = 0;
int posicao_evento = 0;

// Debounce
int leitura_anterior_verde = HIGH;
int leitura_anterior_vermelho = HIGH;
unsigned long ultimo_clique_verde = 0;
unsigned long ultimo_clique_vermelho = 0;

// Aviso de "Fila vazia!"
bool mostrando_erro = false;
unsigned long inicio_erro = 0;

// Wi-Fi e MQTT
bool precisa_publicar = false;
unsigned long ultima_publicacao = 0;
unsigned long ultima_tentativa_wifi = 0;
unsigned long ultima_tentativa_mqtt = 0;
bool wifi_estava_conectado = false;
String ultimo_evento = "INICIO";
String horario_ultimo_evento = "";

// Comando de zerar
bool segurando_dois_botoes = false;
unsigned long inicio_dois_botoes = 0;
bool ja_zerou = false;

// Medição de desempenho
unsigned long voltas_loop = 0;
unsigned long inicio_contagem_loop = 0;


// ----- Horário -----

// Sem NTP, devolve os segundos desde que a placa ligou (ex.: "35s")
String obterHorario() {
  struct tm info;
  if (getLocalTime(&info, 10)) {
    char texto[20];
    strftime(texto, sizeof(texto), "%Y-%m-%d %H:%M:%S", &info);
    return String(texto);
  }
  return String(millis() / 1000) + "s";
}

// Devolve 1 se for horário de pico (10h-12h ou 14h-16h), senão 0.
// Usamos localtime_r em vez de getLocalTime porque ele não fica
// esperando o NTP, o que atrapalharia a medição de desempenho.
int ehHorarioDePico() {
  float hora;

  time_t agora = time(NULL);
  struct tm info;
  localtime_r(&agora, &info);

  if (info.tm_year > (2016 - 1900)) {
    hora = info.tm_hour + info.tm_min / 60.0;
  } else {
    // sem NTP, fingimos que o dia começou às 8h quando a placa ligou
    hora = 8 + (millis() / 1000.0) / 3600.0;
  }

  if ((hora >= 10 && hora < 12) || (hora >= 14 && hora < 16)) {
    return 1;
  }
  return 0;
}


// ----- Log de eventos -----

// Guarda o evento no vetor e imprime uma linha CSV no Serial:
// data_hora,tipo,pessoas_na_fila,atendimentos_concluidos
void registrarEvento(const char* tipo) {
  String agora = obterHorario();

  strcpy(eventos[posicao_evento].tipo, tipo);
  agora.toCharArray(eventos[posicao_evento].horario, 20);
  eventos[posicao_evento].fila = pessoas_na_fila;

  posicao_evento = (posicao_evento + 1) % MAX_EVENTOS;
  if (quantidade_eventos < MAX_EVENTOS) {
    quantidade_eventos++;
  }

  Serial.print(agora);
  Serial.print(",");
  Serial.print(tipo);
  Serial.print(",");
  Serial.print(pessoas_na_fila);
  Serial.print(",");
  Serial.println(atendimentos_concluidos);

  ultimo_evento = tipo;
  horario_ultimo_evento = agora;
  precisa_publicar = true;
}

void guardarTempoAtendimento(float segundos) {
  tempos_atendimento[posicao_tempo] = segundos;
  posicao_tempo = (posicao_tempo + 1) % MAX_TEMPOS;
  if (quantidade_tempos < MAX_TEMPOS) {
    quantidade_tempos++;
  }
}


// ----- Estimativas de espera (em minutos) -----

// Convencional: pessoas na fila x média dos tempos guardados
float esperaConvencional() {
  if (pessoas_na_fila == 0) {
    return 0;
  }

  float media_minutos = 3.0;   // valor usado enquanto não tem histórico

  if (quantidade_tempos > 0) {
    float soma = 0;
    for (int i = 0; i < quantidade_tempos; i++) {
      soma = soma + tempos_atendimento[i];
    }
    media_minutos = (soma / quantidade_tempos) / 60.0;
  }

  return pessoas_na_fila * media_minutos;
}

// IA: espera = A x fila + B x (fila x pico) + C
float esperaIA() {
  if (pessoas_na_fila == 0) {
    return 0;
  }

  int pessoas_no_pico = pessoas_na_fila * ehHorarioDePico();
  float espera = COEF_A * pessoas_na_fila + COEF_B * pessoas_no_pico + COEF_C;

  // se tem alguém na fila, mostramos pelo menos 1 min
  if (espera < 1) {
    espera = 1;
  }
  return espera;
}


// ----- Base numérica -----

// Mostra a fila em decimal, binário e hexadecimal.
// As linhas começam com "#" para não misturar com o log CSV.
void mostrarEmBases(int valor) {
  Serial.print("# Fila em decimal: ");
  Serial.println(valor);
  Serial.print("# Fila em binario: ");
  Serial.println(valor, BIN);
  Serial.print("# Fila em hexadecimal: ");
  Serial.println(valor, HEX);
}


// ----- Flash (memória não volátil) -----

// A Flash tem limite de gravações. Gravar a cada clique serve
// para o protótipo, mas num uso real seria melhor gravar menos.
void salvarNaFlash() {
  memoria_flash.putInt("fila", pessoas_na_fila);
  memoria_flash.putInt("atendidos", atendimentos_concluidos);
}

// Zera a fila, os contadores, os históricos e a Flash
void zerarTudo() {
  pessoas_na_fila = 0;
  atendimentos_concluidos = 0;
  quantidade_tempos = 0;
  posicao_tempo = 0;
  quantidade_eventos = 0;
  posicao_evento = 0;
  ultimo_atendimento_ms = millis();
  memoria_flash.clear();
  registrarEvento("ZERAR");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Tudo zerado!");
  lcd.setCursor(0, 1);
  lcd.print("Solte os botoes");
  Serial.println("# Comando ZERAR: fila, contadores e Flash apagados");
  precisa_publicar = true;
}

// Verde e vermelho apertados juntos por 3 s zeram tudo.
// Cada botão ainda conta um clique ao apertar, mas depois tudo é zerado.
void verificarComandoZerar() {
  bool verde_apertado    = (digitalRead(PINO_BOTAO_VERDE) == LOW);
  bool vermelho_apertado = (digitalRead(PINO_BOTAO_VERMELHO) == LOW);

  if (verde_apertado && vermelho_apertado) {
    if (!segurando_dois_botoes) {
      segurando_dois_botoes = true;
      inicio_dois_botoes = millis();
    }
    if (!ja_zerou && millis() - inicio_dois_botoes >= TEMPO_ZERAR) {
      zerarTudo();
      ja_zerou = true;   // só zera uma vez enquanto segura
    }
  } else {
    if (ja_zerou) {
      atualizarLCD();
    }
    segurando_dois_botoes = false;
    ja_zerou = false;
  }
}


// ----- Desempenho -----

// Uma execução é rápida demais para medir, então rodamos 1000 vezes
// e dividimos por 1000. Usamos fila 10 porque com 0 a função sai antes da conta.
void medirTempoIA() {
  int fila_guardada = pessoas_na_fila;
  pessoas_na_fila = 10;

  volatile float resultado = 0;   // volatile para o compilador não pular a conta
  unsigned long inicio = micros();
  for (int i = 0; i < 1000; i++) {
    resultado = esperaIA();
  }
  unsigned long total_us = micros() - inicio;

  pessoas_na_fila = fila_guardada;

  float media_us = total_us / 1000.0;
  Serial.print("# esperaIA(): ");
  Serial.print(media_us, 3);
  Serial.println(" us por execucao (media de 1000)");
  // ciclos = tempo (us) x frequência (MHz)
  Serial.print("# Ciclos estimados por execucao: ");
  Serial.println(media_us * ESP.getCpuFreqMHz(), 0);
  Serial.print("# (resultado do teste com fila 10: ");
  Serial.print(resultado, 1);
  Serial.println(" min)");
}

// A cada 10 s mostra quantas voltas por segundo o loop deu
void contarVoltasLoop() {
  voltas_loop++;
  unsigned long passou_us = micros() - inicio_contagem_loop;
  if (passou_us >= INTERVALO_DESEMPENHO) {
    float voltas_por_segundo = voltas_loop / (passou_us / 1000000.0);
    Serial.print("# Loop: ");
    Serial.print(voltas_por_segundo, 0);
    Serial.println(" voltas/s");
    voltas_loop = 0;
    inicio_contagem_loop = micros();
  }
}


// ----- LCD -----

// Linha 1: "Fila:X Atend:Y" / Linha 2: "Espera ~Z min" (IA)
void atualizarLCD() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Fila:");
  lcd.print(pessoas_na_fila);
  lcd.print(" Atend:");
  lcd.print(atendimentos_concluidos);

  lcd.setCursor(0, 1);
  lcd.print("Espera ~");
  lcd.print((int) round(esperaIA()));
  lcd.print(" min");
}


// ----- Botões com debounce -----
// Com INPUT_PULLUP, solto = HIGH e apertado = LOW. O clique só conta
// quando passa de HIGH para LOW e já passaram 200 ms do último.

bool botaoVerdeApertado() {
  int leitura = digitalRead(PINO_BOTAO_VERDE);
  bool apertou = false;

  if (leitura == LOW && leitura_anterior_verde == HIGH &&
      millis() - ultimo_clique_verde > TEMPO_DEBOUNCE) {
    apertou = true;
    ultimo_clique_verde = millis();
  }

  leitura_anterior_verde = leitura;
  return apertou;
}

bool botaoVermelhoApertado() {
  int leitura = digitalRead(PINO_BOTAO_VERMELHO);
  bool apertou = false;

  if (leitura == LOW && leitura_anterior_vermelho == HIGH &&
      millis() - ultimo_clique_vermelho > TEMPO_DEBOUNCE) {
    apertou = true;
    ultimo_clique_vermelho = millis();
  }

  leitura_anterior_vermelho = leitura;
  return apertou;
}


// ----- O que cada botão faz -----

// Verde: chegou uma pessoa
void tratarChegada() {
  // se a fila estava vazia, o tempo do próximo atendimento começa agora
  if (pessoas_na_fila == 0) {
    ultimo_atendimento_ms = millis();
  }

  pessoas_na_fila++;
  registrarEvento("ENTRADA");
  salvarNaFlash();
  mostrarEmBases(pessoas_na_fila);
  atualizarLCD();
}

// Vermelho: atendimento concluído
void tratarSaida() {
  // fila vazia: não deixa ficar negativa, acende o LED e mostra o aviso
  if (pessoas_na_fila == 0) {
    digitalWrite(PINO_LED_ERRO, HIGH);
    lcd.setCursor(0, 1);
    lcd.print("Fila vazia!     ");
    mostrando_erro = true;
    inicio_erro = millis();   // o loop apaga depois de 1 s
    registrarEvento("ERRO");
    return;
  }

  pessoas_na_fila--;
  atendimentos_concluidos++;

  // tempo deste atendimento = agora - último atendimento
  unsigned long agora = millis();
  float segundos = (agora - ultimo_atendimento_ms) / 1000.0;
  guardarTempoAtendimento(segundos);
  ultimo_atendimento_ms = agora;

  registrarEvento("SAIDA");
  salvarNaFlash();
  mostrarEmBases(pessoas_na_fila);
  atualizarLCD();
}


// ----- Wi-Fi e MQTT -----

// Tenta reconectar sem travar o programa. O Wi-Fi tenta de novo a cada
// 30 s, sem desligar a tentativa anterior, porque no Wokwi demora para conectar.
void manterConexoes() {
  if (WiFi.status() != WL_CONNECTED) {
    wifi_estava_conectado = false;
    if (millis() - ultima_tentativa_wifi > INTERVALO_WIFI) {
      Serial.println("# Sem Wi-Fi. Nova tentativa de conexao...");
      WiFi.begin(WIFI_NOME, WIFI_SENHA, 6);
      ultima_tentativa_wifi = millis();
    }
    return;   // sem Wi-Fi não adianta tentar o MQTT
  }

  if (!wifi_estava_conectado) {
    Serial.print("# Wi-Fi conectado! IP: ");
    Serial.println(WiFi.localIP());
    wifi_estava_conectado = true;
  }

  // MQTT: tenta a cada 5 s
  if (!clienteMqtt.connected()) {
    if (millis() - ultima_tentativa_mqtt > INTERVALO_RECON) {
      Serial.println("# Conectando ao broker MQTT...");
      // cada cliente precisa de um ID diferente no broker
      String id = "citizenassist-esp32-" + String(random(10000));
      if (clienteMqtt.connect(id.c_str())) {
        Serial.println("# MQTT conectado!");
        precisa_publicar = true;
      } else {
        Serial.print("# Falha no MQTT, codigo: ");
        Serial.println(clienteMqtt.state());
      }
      ultima_tentativa_mqtt = millis();
    }
  }
}

// Monta o JSON e publica. Exemplo:
// {"fila":3,"atendidos":5,"espera_ia_min":4.3,"espera_media_min":6.0,
//  "ultimo_evento":"ENTRADA","horario":"2026-11-14 10:31:05"}
void publicarMQTT() {
  String json = "{";
  json += "\"fila\":";
  json += String(pessoas_na_fila);
  json += ",\"atendidos\":";
  json += String(atendimentos_concluidos);
  json += ",\"espera_ia_min\":";
  json += String(esperaIA(), 1);
  json += ",\"espera_media_min\":";
  json += String(esperaConvencional(), 1);
  json += ",\"ultimo_evento\":\"";
  json += ultimo_evento;
  json += "\",\"horario\":\"";
  json += horario_ultimo_evento;
  json += "\"}";

  // true = mensagem retida, quem abrir o painel depois já recebe o último estado
  clienteMqtt.publish(MQTT_TOPICO, json.c_str(), true);
  Serial.print("# MQTT enviado: ");
  Serial.println(json);
}


// ----- Setup: roda uma vez quando a placa liga -----
void setup() {
  Serial.begin(115200);

  pinMode(PINO_BOTAO_VERDE, INPUT_PULLUP);
  pinMode(PINO_BOTAO_VERMELHO, INPUT_PULLUP);
  pinMode(PINO_LED_ERRO, OUTPUT);
  digitalWrite(PINO_LED_ERRO, LOW);

  // O Wi-Fi tem que começar antes do LCD: no Wokwi, com o LCD primeiro
  // o Wi-Fi não conectava. Esperamos no máximo 30 s.
  Serial.print("# Conectando ao Wi-Fi");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_NOME, WIFI_SENHA, 6);   // canal 6 conecta mais rápido no Wokwi
  unsigned long inicio_wifi = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio_wifi < LIMITE_WIFI) {
    delay(250);
    Serial.print(".");
  }
  ultima_tentativa_wifi = millis();

  // LCD no I2C: SDA = GPIO 21, SCL = GPIO 22
  lcd.init();
  lcd.backlight();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" conectado!");
    Serial.print("# IP: ");
    Serial.println(WiFi.localIP());
    wifi_estava_conectado = true;
  } else {
    // sem rede a fila funciona offline, e o loop tenta conectar de novo
    Serial.println();
    Serial.println("# Sem Wi-Fi: funcionando offline");
    lcd.setCursor(0, 0);
    lcd.print("Sem rede");
    delay(2000);
  }

  // Recupera da Flash a fila salva antes de desligar (0 se nunca salvou)
  memoria_flash.begin("fila", false);
  pessoas_na_fila = memoria_flash.getInt("fila", 0);
  atendimentos_concluidos = memoria_flash.getInt("atendidos", 0);
  Serial.print("# Dados recuperados da Flash: fila ");
  Serial.print(pessoas_na_fila);
  Serial.print(", atendidos ");
  Serial.println(atendimentos_concluidos);

  // Horário pelo NTP, fuso de Brasília (UTC-3)
  configTime(-3 * 3600, 0, "pool.ntp.org");

  clienteMqtt.setServer(MQTT_BROKER, MQTT_PORTA);
  clienteMqtt.setBufferSize(512);
  // O simulador é mais lento que o tempo real e, com 15 s,
  // o broker derrubava a conexão. Por isso usamos 60 s.
  clienteMqtt.setKeepAlive(60);

  // Memória, para o relatório
  Serial.print("# Heap total (RAM): ");
  Serial.print(ESP.getHeapSize());
  Serial.println(" bytes");
  Serial.print("# Heap livre (RAM): ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");
  Serial.print("# Tamanho da Flash: ");
  Serial.print(ESP.getFlashChipSize());
  Serial.println(" bytes");
  Serial.print("# Tamanho do programa (sketch): ");
  Serial.print(ESP.getSketchSize());
  Serial.println(" bytes");
  Serial.print("# Espaco livre para programa: ");
  Serial.print(ESP.getFreeSketchSpace());
  Serial.println(" bytes");

  // Desempenho, para o relatório
  Serial.print("# Frequencia do clock da CPU: ");
  Serial.print(ESP.getCpuFreqMHz());
  Serial.println(" MHz");
  medirTempoIA();
  inicio_contagem_loop = micros();

  // Cabeçalho do log CSV (linhas com # são só mensagens)
  Serial.println("data_hora,tipo,pessoas_na_fila,atendimentos_concluidos");

  horario_ultimo_evento = obterHorario();
  ultimo_atendimento_ms = millis();
  atualizarLCD();
  precisa_publicar = true;
}


// ----- Loop -----
void loop() {
  manterConexoes();
  clienteMqtt.loop();

  if (botaoVerdeApertado()) {
    tratarChegada();
  }
  if (botaoVermelhoApertado()) {
    tratarSaida();
  }
  verificarComandoZerar();

  // apaga o aviso de erro depois de 1 s
  if (mostrando_erro && millis() - inicio_erro >= TEMPO_ERRO) {
    digitalWrite(PINO_LED_ERRO, LOW);
    mostrando_erro = false;
    atualizarLCD();
  }

  // publica quando algo mudou ou a cada 10 s
  if (clienteMqtt.connected()) {
    if (precisa_publicar || millis() - ultima_publicacao >= INTERVALO_MQTT) {
      publicarMQTT();
      precisa_publicar = false;
      ultima_publicacao = millis();
    }
  }

  contarVoltasLoop();

  // Digitar "r" no Serial reinicia a placa. A RAM é apagada, mas a Flash
  // mantém a fila. Fizemos isso porque o botão EN não reinicia no Wokwi.
  if (Serial.available() > 0) {
    char comando = Serial.read();
    if (comando == 'r' || comando == 'R') {
      Serial.println("# Reiniciando a placa (teste de persistencia)...");
      delay(500);
      ESP.restart();
    }
  }
}
