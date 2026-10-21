<<<<<<< HEAD
# Medidor de distância ultrassônico com envio por LoRaWAN
 
Firmware para ESP32 que lê um sensor ultrassônico **AJ-SR04M** (modo 1, com o resistor R19 aberto) e transmite a distância medida por **LoRaWAN** (ativação **ABP**), usando a biblioteca **LMIC** e o formato de payload **Cayenne LPP**.
 
O caso de uso típico é medição de nível (reservatório, caixa d'água, silo) com transmissão periódica para uma rede LoRaWAN como a TTN/Helium/ChirpStack.
 
---
 
## Hardware
 
### Ligações do sensor
 
| Sensor AJ-SR04M | ESP32 |
|---|---|
| `5V` | `5V` |
| `GND` | `GND` |
| `Trig` | `GPIO 13` (com resistor de 10 kΩ para GND) |
| `Echo` | passa por resistor de 1 kΩ → `GPIO 25` → resistor de 2 kΩ → `GND` , pulldown|
 
O pulldown no pino `Echo` converte o sinal de 5 V do sensor para os ~3,3 V que o ESP32 aceita.
 
O resistor R19 do sensor fica **aberto**, o que coloca a placa no **modo 1** (operação clássica trigger/echo, igual ao HC-SR04).
 
### Pinagem do rádio LoRa
 
A pinagem segue o padrão das placas ESP32 LoRa (ex.: TTGO/Heltec):
 
```
NSS  = 18     RST  = 23
SCK  = 5      MOSI = 27     MISO = 19
DIO0 = 26     DIO1 = 33     DIO2 = 32
```
 
---
 
## Como o código funciona
 
### 1. Leitura do sensor
 
A função `medirPulsoUs()` dispara um pulso curto no `Trig` e usa `pulseIn()` para medir, em microssegundos, quanto o pino `Echo` fica em nível alto. Esse tempo corresponde ao vai-e-volta do som até o obstáculo.
 
Para evitar ruído, `distanciasend()` faz várias leituras seguidas (`AMOSTRAS = 12`) e classifica cada uma em quatro categorias:
 
- **Travado** — pulso nunca terminou (retorno `-1`).
- **Sem eco** — pulso ausente ou maior que `ECHO_MAX_US` .
- **Zona cega** — pulso menor que `ECHO_MIN_US` (~20 cm, limite físico do sensor).
- **Válida** — tudo entre esses dois limites.
As amostras válidas são **ordenadas** e descarta-se o primeiro e o último quarto, para então pegar a **mediana** da parte central. Isso remove a maioria dos picos esporádicos sem precisar de filtros mais pesados.
 
A distância em metros sai de:
 
```
distancia = tempo_us * velocidade_som / 2.000.000
```
 
com `velocidadeSom = 346,45 m/s` (ar a ~25 °C).
 
### 2. Rejeição de outliers
 
Se a nova medida pular mais de **30 cm** em relação à última aceita, ela é tratada como suspeita. O código tolera até `MAX_REJEICOES ` rejeições seguidas; depois disso aceita a leitura mesmo com salto grande (para não ficar preso caso o nível realmente tenha mudado rápido).
 
### 3. Montagem do payload (Cayenne LPP)
 
A distância, em metros, vai para o **canal 1** como `Analog Input` (resolução de 0,01 m). No modo de teste (`#define TESTE`), um segundo valor é adicionado no **canal 2** só para identificar a fonte de alimentação durante os testes de bancada.
 
O buffer resultante é copiado para `payloadRecebido[]` e marcado como pronto para envio.
 
### 4. Controle do ritmo de envio
 
O envio é temporizado por `tickSendIfDue()`, chamada a cada iteração do `loop()`:
 
- Os **primeiros `ENVIOS_INICIAIS` (5)** pacotes saem a cada `INTERVALO_INICIAL_S` segundos (10 s) — útil para validar rapidamente que o dispositivo está conectado no gateway.
- Depois disso, o intervalo passa a `INTERVALO_NORMAL_S` segundos, que é o ritmo operacional.
Se, no momento de enviar, a leitura falhar:
 
- Com `SEND_OLD_WHEN_NO_NEW_READING = true` (padrão), o último valor válido é **reenviado**.
- Com `false`, o envio é simplesmente pulado.
A espera entre amostras (`esperaComLmic`) não bloqueia o LMIC — ela continua chamando `os_runloop_once()` para o stack LoRa não travar.
 
### 5. LoRaWAN (ABP)
 
A ativação é por **ABP** (chaves fixas gravadas no código): `NWKSKEY`, `APPSKEY` e `DEVADDR`. Há dois jogos de chaves separados por `#define TESTE` — basta comentar essa linha para compilar com as chaves de produção.
 
Os ajustes principais do LMIC:
 
- `LMIC_setDrTxpow(DR_SF10, 14)` — SF10, 14 dBm.
- `LMIC.dn2Dr = DR_SF7` — datarate da janela RX2.
- Só a sub-banda ativa fica habilitada (canais 0–6 e 16–62 são desligados), o que é o padrão para redes AU915/US915 que usam a sub-banda 2 (canais 8–15).
- `LMIC_setLinkCheckMode(0)` — sem link check.
A transmissão em si é feita por `LMIC_setTxData2(1, payloadRecebido, tamanhoPayload, 0)` — porta 1, **unconfirmed uplink**.
 
A função `onEvent()` imprime o evento recebido do LMIC e, em `EV_TXCOMPLETE`, mostra qualquer downlink que tenha chegado.
 
---
 
## Parâmetros ajustáveis
 
Na parte de cima do arquivo:
 
| Parâmetro | Função |
|---|---|
| `ENVIOS_INICIAIS` | Quantos pacotes mandar no ritmo acelerado |
| `INTERVALO_INICIAL_S` | Intervalo entre os envios iniciais (s) |
| `INTERVALO_NORMAL_S` | Intervalo entre os envios normais (s) |
| `SEND_OLD_WHEN_NO_NEW_READING` | Reenviar último valor quando a leitura falhar |
| `AMOSTRAS` | Nº de leituras por ciclo (mediana entre elas) |
| `ECHO_MIN_US` / `ECHO_MAX_US` | Limites de pulso aceitos |
| `MAX_REJEICOES` | Quantos outliers consecutivos ignorar antes de aceitar |
| `velocidadeSom` | Velocidade do som usada no cálculo |
| `#define TESTE` | Alterna entre chaves de teste e de produção |
 
---
 
## Dependências
 
- [MCCI LMIC](https://github.com/mcci-catena/arduino-lmic) (configurar a região correta no `lmic_project_config.h`, ex. `CFG_au915`).
- [CayenneLPP](https://github.com/ElectronicCats/CayenneLPP).
- Core ESP32 para Arduino.
---
 
## Saída no monitor serial
 
Durante a operação, o monitor (115200 baud) mostra algo assim:
 
```
Distancia: 1.42 m | pulso: 8204 us | validas: 10/12 | faixa: 8120-8290 us
Payload LPP preparado (4 bytes)
---
Tick: enfileirando TX LoRa...
Canal: 10
Freq : 916800000
DR   : 2
Tick: Packet queued. Envio #3 | proximo em 10 s
```
 
Quando o pacote é transmitido, aparece `EV_TXCOMPLETE`, e qualquer downlink recebido do servidor é impresso em sequência.
=======
# AGRO_ULTRASOM_LORA



## Getting started

To make it easy for you to get started with GitLab, here's a list of recommended next steps.

Already a pro? Just edit this README.md and make it your own. Want to make it easy? [Use the template at the bottom](#editing-this-readme)!

## Add your files

- [ ] [Create](https://docs.gitlab.com/ee/user/project/repository/web_editor.html#create-a-file) or [upload](https://docs.gitlab.com/ee/user/project/repository/web_editor.html#upload-a-file) files
- [ ] [Add files using the command line](https://docs.gitlab.com/topics/git/add_files/#add-files-to-a-git-repository) or push an existing Git repository with the following command:

```
cd existing_repo
git remote add origin https://gitlab.agrocitylivinglab.com.br/agrocity/agro_ultrasom_lora.git
git branch -M main
git push -uf origin main
```

## Integrate with your tools

- [ ] [Set up project integrations](https://gitlab.agrocitylivinglab.com.br/agrocity/agro_ultrasom_lora/-/settings/integrations)

## Collaborate with your team

- [ ] [Invite team members and collaborators](https://docs.gitlab.com/ee/user/project/members/)
- [ ] [Create a new merge request](https://docs.gitlab.com/ee/user/project/merge_requests/creating_merge_requests.html)
- [ ] [Automatically close issues from merge requests](https://docs.gitlab.com/ee/user/project/issues/managing_issues.html#closing-issues-automatically)
- [ ] [Enable merge request approvals](https://docs.gitlab.com/ee/user/project/merge_requests/approvals/)
- [ ] [Set auto-merge](https://docs.gitlab.com/user/project/merge_requests/auto_merge/)

## Test and Deploy

Use the built-in continuous integration in GitLab.

- [ ] [Get started with GitLab CI/CD](https://docs.gitlab.com/ee/ci/quick_start/)
- [ ] [Analyze your code for known vulnerabilities with Static Application Security Testing (SAST)](https://docs.gitlab.com/ee/user/application_security/sast/)
- [ ] [Deploy to Kubernetes, Amazon EC2, or Amazon ECS using Auto Deploy](https://docs.gitlab.com/ee/topics/autodevops/requirements.html)
- [ ] [Use pull-based deployments for improved Kubernetes management](https://docs.gitlab.com/ee/user/clusters/agent/)
- [ ] [Set up protected environments](https://docs.gitlab.com/ee/ci/environments/protected_environments.html)

***

# Editing this README

When you're ready to make this README your own, just edit this file and use the handy template below (or feel free to structure it however you want - this is just a starting point!). Thanks to [makeareadme.com](https://www.makeareadme.com/) for this template.

## Suggestions for a good README

Every project is different, so consider which of these sections apply to yours. The sections used in the template are suggestions for most open source projects. Also keep in mind that while a README can be too long and detailed, too long is better than too short. If you think your README is too long, consider utilizing another form of documentation rather than cutting out information.

## Name
Choose a self-explaining name for your project.

## Description
Let people know what your project can do specifically. Provide context and add a link to any reference visitors might be unfamiliar with. A list of Features or a Background subsection can also be added here. If there are alternatives to your project, this is a good place to list differentiating factors.

## Badges
On some READMEs, you may see small images that convey metadata, such as whether or not all the tests are passing for the project. You can use Shields to add some to your README. Many services also have instructions for adding a badge.

## Visuals
Depending on what you are making, it can be a good idea to include screenshots or even a video (you'll frequently see GIFs rather than actual videos). Tools like ttygif can help, but check out Asciinema for a more sophisticated method.

## Installation
Within a particular ecosystem, there may be a common way of installing things, such as using Yarn, NuGet, or Homebrew. However, consider the possibility that whoever is reading your README is a novice and would like more guidance. Listing specific steps helps remove ambiguity and gets people to using your project as quickly as possible. If it only runs in a specific context like a particular programming language version or operating system or has dependencies that have to be installed manually, also add a Requirements subsection.

## Usage
Use examples liberally, and show the expected output if you can. It's helpful to have inline the smallest example of usage that you can demonstrate, while providing links to more sophisticated examples if they are too long to reasonably include in the README.

## Support
Tell people where they can go to for help. It can be any combination of an issue tracker, a chat room, an email address, etc.

## Roadmap
If you have ideas for releases in the future, it is a good idea to list them in the README.

## Contributing
State if you are open to contributions and what your requirements are for accepting them.

For people who want to make changes to your project, it's helpful to have some documentation on how to get started. Perhaps there is a script that they should run or some environment variables that they need to set. Make these steps explicit. These instructions could also be useful to your future self.

You can also document commands to lint the code or run tests. These steps help to ensure high code quality and reduce the likelihood that the changes inadvertently break something. Having instructions for running tests is especially helpful if it requires external setup, such as starting a Selenium server for testing in a browser.

## Authors and acknowledgment
Show your appreciation to those who have contributed to the project.

## License
For open source projects, say how it is licensed.

## Project status
If you have run out of energy or time for your project, put a note at the top of the README saying that development has slowed down or stopped completely. Someone may choose to fork your project or volunteer to step in as a maintainer or owner, allowing your project to keep going. You can also make an explicit request for maintainers.
>>>>>>> 1ffdb4debe6ba0f41829a4ce49a4440015c0101d
