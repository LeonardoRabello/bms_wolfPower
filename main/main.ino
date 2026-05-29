#include <INA219.h>
#include <Wire.h>

INA219 INA1(0x40);
INA219 INA2(0x41);
INA219 INA3(0x44);
INA219 INA4(0x45);

INA219* sensores[] = {&INA1, &INA2, &INA3, &INA4};
const char* nomes[] = {"Bateria 1", "Bateria 2", "Bateria 3", "Bateria 4"};

struct Medicao {
  int bateria;
  float tensao;
  unsigned long time;
}

Medicao medir(int i) {
  Medicao m;
  m.bateria = 1;
  m.tensao   = sensores[i]->getBusVoltage();
  m.corrente = sensores[i]->getCurrent_mA();
  m.time = milis();
  return m;
}
/*
void balancear() {
  for (int i = 0, i < 4, I++) {
    if (sensores[i]->getBusVoltage() > media) {

    }
  }
}
*/
void setup() {
  Serial.begin(115200);
  Wire.begin();

  for (int i = 0; i < 4; i++) {
    if (!sensores[i]->begin()) {
      Serial.print(nomes[i]);
      Serial.println(": INA219 não encontrado!");
    } else {
      sensores[i]->setMaxCurrentShunt(2, 0.1);
      Serial.print(nomes[i]);
      Serial.println(": iniciado com sucesso!");
    }
  }
}

void loop() {

}