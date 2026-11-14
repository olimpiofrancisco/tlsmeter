# TLS Meter — Calibração com Amostra Padrão

**Entrega dos fontes de desenvolvimento (não homologada em hardware)**
- IHM: `Display/TLS_Meter_Display_V2_0_Calibracao/TLS_Meter_Display_V2_0_Calibracao.ino`
- Biblioteca de ícones: `Display/TLS_Meter_Display_V2_0_Calibracao/tls_assets.h`
- Windows: `Windows/tls_windows_app_v6_calibracao.py`
- Armazenamento e cálculos Python: `Windows/calibration_store.py`

## O que foi implementado

### Na IHM (firmware 2.0)
1. Selecionar o perfil **Laboratório** em `AJUSTES > ENSAIO` (botão `TROCAR`).
2. Abrir `AJUSTES > ENSAIO > CALIBRAÇÃO`.
3. Informar a condutividade de referência **da ficha da Amostra Padrão** com os botões `-` e `+`. Toque no botão numérico central para alternar o passo entre 0,1; 0,01; 0,001 e 0,0001 W/(m·K); use `ZERAR` para recomeçar. O valor inicial é zero: não existe referência genérica pré-inserida.
4. Posicionar a sonda no corpo de prova. Pressionar `ENS. PADRÃO` (inicia o mesmo ciclo de aquisição que já existia).
5. Ao término, a IHM apresenta `k_ref`, `k_bruto`, o fator proposto `F` e `R²`. Conferir e escolher `CONFIRMAR` ou `DESCARTAR`.
6. Após confirmação, o fator é salvo na memória não volátil (`Preferences` da IHM) e é aplicado **somente aos novos ensaios**. A IHM permite desativar/reativar e restaurar o fator.
7. Os arquivos de ensaio gravados no microSD registram, quando disponível, `RHO_RAW`, `K_RAW`, `CAL_FACTOR_APPLIED`, resultados apresentados e identificação de ensaio de calibração.

### No Windows (app 6.0)
1. Na coluna lateral, selecionar **Laboratório — IEEE 442**.
2. Abrir a nova aba **Calibração**.
3. Informar a identificação da Amostra Padrão (conforme sua ficha) e `k_ref` em W/(m·K). Não utilizar valor genérico de Nylon 6.6.
4. Pressionar `Executar ensaio de calibração`; o programa segue o ciclo completo e volta à aba de calibração ao terminar.
5. O aplicativo mostra a proposta `F = k_ref / k_bruto`, sem ativá-la automaticamente. Conferir a qualidade do ensaio e usar `Confirmar fator calculado`, ou descartar.
6. Após confirmação, o fator fica em `~/.tls_meter/calibration_windows_v6.json` no computador atual, podendo ser desativado, reativado ou apagado.
7. Os próximos ensaios e planilhas Excel mostram e registram os valores brutos, valores corrigidos e o fator associado ao início do ciclo. Um fator alterado depois não recalcula um ensaio antigo.

## Equações utilizadas

- Fator: `F = k_ref / k_bruto`
- Condutividade apresentada: `k_corr = F * k_bruto`
- Resistividade apresentada: `rho_corr = rho_bruto / F`
- `R²`, temperatura, potência e dados brutos não são alterados.

**Atenção:** a IHM e o Windows usam fatores *independentes*; não existe transferência automática de calibração via Bluetooth. O firmware do ESP32 de medição **não foi alterado** e continua aplicando as proteções originais. O fator de correção também não prova, isoladamente, conformidade metrológica com a IEEE 442.

## Instalação

### IHM
1. Abrir a pasta `TLS_Meter_Display_V2_0_Calibracao` na Arduino IDE. O arquivo `.ino` e o `tls_assets.h` devem permanecer juntos.
2. Usar a mesma configuração de placa ESP32 e bibliotecas da versão V1.9 já instalada (Adafruit GFX, Adafruit ST7735/ST7789, BluetoothSerial, Preferences, SD).
3. Compilar e enviar à placa. Este pacote **não altera** a placa medidora nem o protocolo Bluetooth.
4. **Validar no hardware** o toque, os limites do menu, reconexão e persistência depois de reiniciar.

### Windows
1. Instalar Python 3.9 compatível com as bibliotecas utilizadas pela V5.
2. Abrir um terminal na pasta `Windows`.
3. Instalar: `python -m pip install -r requirements.txt`.
4. Iniciar: `python tls_windows_app_v6_calibracao.py`, ou executar `iniciar_windows.bat`.
5. Liberar o medidor pelo menu PC da IHM antes de usar a conexão Bluetooth pelo Windows.

## Testes efetuados neste ambiente

- Python: compilação estática (`py_compile`) concluída.
- Módulo de calibração: 13 testes automatizados de cálculo, validação, persistência e rejeição de dados inválidos aprovados.
- IHM: revisão estática do código e integração dos estados/telas. **Não foi possível compilar o firmware ESP32** neste ambiente (sem toolchain Arduino/ESP32).
- Windows: **não foi possível executar a interface gráfica nem testar a porta COM**, pois o ambiente não contém as dependências GUI e o hardware do TLS Meter.

## Teste de aceitação recomendado no equipamento

1. Iniciar sem calibração: conferir `F=1`.
2. Calibrar com valor da ficha da Amostra Padrão e confirmar; reiniciar e conferir persistência.
3. Medir novamente a mesma peça, comparar valores brutos/corrigidos.
4. Desativar fator e verificar retorno aos valores brutos apenas em ciclos novos.
5. Interromper um ensaio de calibração: não deve salvar fator novo.
6. Testar desconexão Bluetooth no aquecimento: firmware medidor deve desligar o aquecedor e abortar.
7. Salvar um ensaio em SD/Excel; conferir campos brutos e corrigidos.
8. Repetir no Windows (independentemente da IHM) e conferir o JSON depois de reiniciar o app.
