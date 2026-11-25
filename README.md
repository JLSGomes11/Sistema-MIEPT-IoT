# Sistema de Monitoramento da Integridade Estrutural de Pontes e Trapiches

Disciplina: Internet das Coisas - Profª Salete Farias

Aluno(a): **Jorge Lucas da Silva Gomes**

> Projeto derivado do trabalho original de [Franciele Alves](https://github.com/frnclz/IOT---SMIEPT), com melhorias de confiabilidade e segurança.

## Descrição do Problema

Pontes e trapiches localizados em ambientes marítimos estão constantemente expostos a condições adversas, como ação das ondas, marés, ressacas e corrosão causada pela água salgada. Esses fatores podem provocar deformações, vibrações excessivas e inclinações que comprometem a integridade estrutural ao longo do tempo.

A ausência de monitoramento contínuo dificulta a identificação precoce de falhas, aumentando o risco de acidentes e reduzindo a eficiência das ações de manutenção preventiva.

Este projeto propõe um sistema de monitoramento estrutural baseado em Internet das Coisas (IoT), capaz de acompanhar em tempo real as condições da estrutura e acionar mecanismos de segurança quando forem detectadas situações de risco.

## Objetivo

Desenvolver um sistema capaz de monitorar inclinações em pontes ou trapiches, analisando continuamente os dados coletados por sensores e executando ações automáticas de segurança quando necessário, mesmo sem conexão com a internet.

## Componentes Utilizados

| Componente | Quantidade | Função |
|---|---|---|
| ESP32 | 1 | Microcontrolador com Wi-Fi |
| MPU6050 | 2 | Medem a inclinação dos pilares 1 e 3 |
| Servo Motor SG90 | 1 | Aciona a cancela de segurança |
| Protoboard | 1 | Montagem do circuito |
| Jumpers | vários | Ligações |
| Fonte de alimentação | 1 | Alimentação do sistema |

## Ligações

| Dispositivo | Pino do dispositivo | Pino do ESP32 |
|---|---|---|
| MPU6050 (Pilar 1) | SDA | GPIO 21 |
| MPU6050 (Pilar 1) | SCL | GPIO 22 |
| MPU6050 (Pilar 3) | SDA | GPIO 19 |
| MPU6050 (Pilar 3) | SCL | GPIO 4 |
| MPU6050 (ambos) | VCC / GND | 3.3V / GND |
| Servo SG90 | Sinal | GPIO 18 |

Cada MPU6050 usa um barramento I2C separado (`Wire` e `Wire1`), o que evita conflito de endereço entre os dois sensores.

## Funcionamento do Sistema

O ESP32 lê os dois sensores a cada 100 ms, calcula a inclinação de cada pilar e decide o estado da estrutura. **Essa decisão é feita localmente, antes de qualquer comunicação de rede**, então a cancela continua funcionando mesmo se o Wi-Fi ou o broker cair.

### Tratamento das leituras

* **Calibração:** ao ligar, a posição atual de cada sensor é tomada como zero (média de 30 leituras).
* **Média móvel:** as últimas 8 leituras são suavizadas para evitar falsos alarmes por vibração ou ondas.
* **Histerese:** o sistema só volta a um nível mais seguro quando a inclinação cai 1° abaixo do limite, evitando que a cancela abra e feche repetidamente perto do limite.

### Níveis de alerta

| Nível | Condição | Cancela |
|---|---|---|
| Normal | Inclinação abaixo de 2,5° | Aberta |
| Atenção | Entre 2,5° e 4° | Aberta |
| Perigo | Acima de 4° | **Fechada** |
| Falha de sensor | Sensor sem resposta, travado ou com leitura inválida | **Fechada** (fail-safe) |

### Ação de segurança

Quando a inclinação ultrapassa o limite de perigo ou um sensor falha:

* A cancela é fechada automaticamente através do servo motor;
* O acesso à estrutura é bloqueado;
* Um alerta é publicado via MQTT e exibido no dashboard;
* O evento é registrado no painel para análise.

A cancela sempre **inicia fechada** ao ligar o sistema e só abre depois que os sensores confirmam que está tudo certo.

## Fluxo do Sistema

1. Os sensores coletam dados de inclinação.
2. O ESP32 filtra as leituras e verifica falhas de sensor.
3. O sistema calcula o nível (normal, atenção, perigo ou falha) aplicando a histerese.
4. A cancela é aberta ou fechada de acordo com o nível.
5. Os dados são publicados via MQTT (a cada 1 s e imediatamente quando o nível muda).
6. O dashboard recebe os dados em tempo real e exibe status, gráfico e registro de eventos.

## Comunicação MQTT

Broker: `broker.emqx.io` (porta 1883 no ESP32 e 8084 com SSL no navegador).

| Tópico | Sentido | Conteúdo |
|---|---|---|
| `[SEU_PREFIXO]/sensores` | ESP32 → Dashboard | Leituras e estado do sistema (JSON) |
| `[SEU_PREFIXO]/status` | ESP32 → Dashboard | `online` (com motivo do último reinício) ou `offline` (Last Will) |
| `[SEU_PREFIXO]/alerta` | Dashboard → Dashboard | Alerta manual de ponte interditada |

Exemplo de mensagem em `sensores`:

```json
{
  "angulo1": 0.52,
  "angulo2": -0.31,
  "nivel": "normal",
  "risco": false,
  "cancela": "aberta",
  "sensor1": "ok",
  "sensor2": "ok"
}
```

> Os tópicos precisam ser **idênticos** no `secrets.h` do ESP32 e no topo do `dashboard/js/app.js`.

## Instalação

### Bibliotecas necessárias (Arduino IDE)

* Adafruit MPU6050
* Adafruit Unified Sensor
* ESP32Servo
* PubSubClient

### Configuração do ESP32

1. Monte o circuito conforme a tabela de ligações.
2. Copie `secrets.example.h` para `secrets.h` e preencha com os dados da sua rede e os tópicos MQTT.
3. Abra `ponte_monitor/ponte_monitor.ino` na Arduino IDE (a pasta do sketch deve ter o mesmo nome do arquivo).
4. Selecione a placa ESP32 e faça o upload.
5. Abra o Monitor Serial (115200) para acompanhar os dados.

> O arquivo `secrets.h` está no `.gitignore` e **não deve ser enviado ao GitHub**.

### Publicação do dashboard (Vercel)

1. Suba a pasta `dashboard` para o GitHub (o `index.html` deve ficar na raiz do projeto publicado).
2. No Vercel, importe o repositório (Framework Preset: `Other`, sem comando de build).
3. Clique em **Deploy**.

## Utilização

Após a inicialização:

1. Os dois MPU6050 são calibrados e a cancela abre se estiver tudo normal.
2. O dashboard mostra a inclinação de cada pilar, o status da ponte e da cancela.
3. Se a inclinação passar do limite, a cancela fecha e o dashboard sinaliza o risco.
4. Quando a inclinação volta para a faixa segura (com histerese), a cancela reabre.
5. Se um sensor falhar, a cancela fecha por segurança e o dashboard indica qual pilar falhou.

### Como testar

| Teste | Resultado esperado |
|---|---|
| Inclinar a estrutura até cerca de 3° | Nível "Atenção", cancela continua aberta |
| Passar de 4° | Nível "Perigo", cancela fecha |
| Voltar para cerca de 3,5° | Cancela continua fechada (histerese) |
| Voltar abaixo de 3° | Cancela reabre |
| Desconectar o SDA de um MPU | "Falha de sensor" em cerca de 0,3 s, cancela fecha |
| Desligar o Wi-Fi | Cancela continua reagindo; dashboard mostra dispositivo offline |

## Circuito

<!-- TROCAR: coloque a imagem do circuito em docs/img/circuito.png -->
![Circuito do projeto](docs/img/circuito.png)

Acesse o [circuito no Wokwi](COLE_O_LINK_DO_WOKWI_AQUI) para visualizar a simulação do projeto.

## Dashboard

<!-- TROCAR: prints do dashboard em docs/img/ -->
**Estado normal**

![Dashboard em estado normal](docs/img/dashboard-normal.png)

**Estado de risco (cancela fechada)**

![Dashboard em estado de risco](docs/img/dashboard-risco.png)

**Histórico de inclinação e registro de eventos**

![Gráfico e registro de eventos](docs/img/dashboard-historico.png)

Acesse o [dashboard do projeto](COLE_O_LINK_DO_VERCEL_AQUI) para visualizar em tempo real.

### Recursos do dashboard

* Inclinação de cada pilar, com cor por nível (verde, amarelo, vermelho)
* Status da ponte e da cancela
* Status do dispositivo (online/offline) com detecção de falta de dados
* Gráfico de histórico com as linhas de limite de atenção e de risco
* Registro de eventos (mudanças de nível, dispositivo offline/online, reinícios)
* Botão de alerta manual de ponte interditada

## Sistema Físico

<!-- TROCAR: fotos da montagem física em docs/img/ -->
![Visão geral da montagem](docs/img/sistema-fisico-1.jpg)

![Detalhe da cancela e dos sensores](docs/img/sistema-fisico-2.jpg)

## Vídeo do Funcionamento do Sistema

[Assistir ao vídeo](COLE_O_LINK_DO_VIDEO_AQUI)

## Confiabilidade e Segurança

O estudo dirigido sobre confiabilidade e segurança do projeto está em [`docs/estudo_dirigido_confiabilidade_seguranca_iot.pdf`](docs/estudo_dirigido_confiabilidade_seguranca_iot.pdf).

Riscos identificados e como foram tratados:

| Risco | Tratamento aplicado |
|---|---|
| Queda de Wi-Fi/MQTT travava a cancela | Segurança roda antes da rede; reconexão não bloqueante |
| Credenciais no código | Movidas para `secrets.h` (fora do GitHub) |
| Broker público sem autenticação | Tópico exclusivo e validação das mensagens no dashboard (TLS e login previstos) |
| Falha ou ruído de sensor | Detecção de falha com fail-safe, média móvel e histerese |
| Queda de energia | Cancela inicia fechada; motivo do reinício publicado via MQTT |

## Implementações Realizadas

### v1
* Monitoramento de inclinação da estrutura
* Sistema de bloqueio de segurança (cancela)
* Processamento local dos dados

### v2
* Integração do protocolo MQTT para comunicação
* Envio de alertas remotos em situações de risco
* Dashboard para monitoramento em tempo real

### v3
* Segundo sensor MPU6050 em barramento I2C separado
* Média móvel e histerese
* Níveis de alerta (normal, atenção, perigo, falha de sensor)
* Detecção de falha de sensor com comportamento fail-safe
* Rede não bloqueante, Last Will e status do dispositivo
* Credenciais fora do código
* Dashboard com gráfico de histórico, registro de eventos e status online/offline

## Aplicações Futuras

* Armazenamento histórico em banco de dados (hoje o histórico fica só no navegador)
* Broker MQTT com TLS e autenticação (ex.: HiveMQ Cloud)
* Extensômetro com módulo HX711 para medir deformação (hoje só a inclinação é monitorada)
* Notificações por Telegram ou e-mail
* Comandos remotos autenticados (abrir/fechar cancela, ajustar limites)
* Watchdog e alimentação separada para o servo

## Estrutura do Repositório

```
.
├── ponte_monitor/
│   ├── ponte_monitor.ino
│   ├── secrets.example.h
│   └── (secrets.h  -> não versionado)
├── dashboard/
│   ├── index.html
│   ├── css/style.css
│   └── js/app.js
├── docs/
│   ├── estudo_dirigido_confiabilidade_seguranca_iot.pdf
│   └── img/           
├── .gitignore
└── README.md
```
