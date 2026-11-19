# Diagrama de arquitetura

```mermaid
flowchart LR
    subgraph ENTRADAS
        BV["Botão verde<br/>chegou pessoa<br/>GPIO 18"]
        BR["Botão vermelho<br/>atendido<br/>GPIO 19"]
    end

    subgraph ESP32["ESP32 DevKit C V4"]
        CPU["CPU Xtensa LX6<br/>dual-core, 240 MHz"]
        IRAM["IRAM<br/>instruções"]
        DRAM["DRAM<br/>variáveis da fila,<br/>tempos e log de eventos"]
        CACHE["Cache"]
        FLASH["Flash 4 MB<br/>programa + fila salva"]
    end

    subgraph SAIDAS["SAÍDAS"]
        LCD["LCD 16x2<br/>I2C (GPIO 21 e 22)"]
        LED["LED de erro<br/>GPIO 2"]
        SER["Serial<br/>log CSV"]
    end

    subgraph REDE
        WIFI["Wi-Fi"]
        BROKER["Broker MQTT<br/>broker.hivemq.com"]
        WEB["Painel no navegador"]
    end

    BV --> CPU
    BR --> CPU
    IRAM -- instruções --> CPU
    CPU <-- dados --> DRAM
    FLASH --> CACHE --> CPU
    CPU -- Preferences --> FLASH
    CPU --> LCD
    CPU --> LED
    CPU --> SER
    CPU --> WIFI
    WIFI -- "MQTT (TCP 1883)" --> BROKER
    BROKER -- "MQTT por WebSocket (8884)" --> WEB
```

Quando alguém aperta um botão, o pino vai de HIGH para LOW e a CPU trata o clique: atualiza a fila na DRAM, salva a fila na Flash e calcula a espera. Depois disso ela atualiza o LCD pelo I2C, escreve o evento no Serial e, se estiver conectada, publica um JSON no broker MQTT. O painel está inscrito no mesmo tópico e mostra os dados assim que recebe.

As instruções e os dados chegam à CPU por caminhos separados (IRAM e DRAM), por isso o ESP32 é considerado Harvard modificada. Isso está explicado no relatório, na parte de análise da arquitetura.
