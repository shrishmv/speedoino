/* Speedoino - This file is part of the firmware.
 * Copyright (C) 2011 Kolja Windeler
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "global.h"

speedo_dz::speedo_dz(void){
}

void speedo_dz::counter(){
	if(DZ_DEBUG){
		Serial.print("DZ peak@");
		Serial.print(millis());
		Serial.print(" ");
		Serial.println(peak_count);
	};
	peak_count++; // läuft bis 65.536 dann auf 0
};

void speedo_dz::calc() {
	///// DZ BERECHNUNG ////////
	unsigned long now=millis(); 		// aktuelle zeit
	unsigned long differ=now-previous_time; // zeit seit dem letzte mal abholen der daten
	unsigned int  now_peaks=peak_count; // aktueller dz zähler stand, separate var damit der peakcount weiter verndert werden koennte
	if(now_peaks>15 && differ>250){ // max mit 10Hz, bei niedriger drehzahl noch seltener, 1400 rpm => 680 ms
		//now_peaks=now_peaks>>anzahl_shift;			// könnte ja sein das man weniger umdrehungen als funken hat, hornet hat 2 Funken je Umdrehun
		/* bei 15krpm = 25 peaks
		 * differ => 100 --> 60000/100=600
		 * 600 * 25 => 15.000
		 *
		 * bei 1400rpm => 685 ms fr 16 Peaks
		 * 60000/685 = 88
		 * 88 * 16 = 1408
		 */
		unsigned int dz=60000/differ*now_peaks/2; // Drehzahl berechnet (60.000 weil ms => min)
		if(dz>15000){ // wenn man über 15000 U/min => Abstand von 2 Zündungen = 60000[ms/min]/15000[U/min] = 4 [ms/U]
			dz=previous_dz; // alten Wert halten, kann nicht sein
		};

		peak_count=0;
		previous_time=now; // speichere wann ich zuletzt nachgesehen habe
		previous_dz=dz; // speichere die aktuelle dz

		exact=dz;
		rounded=250*round(dz/250); // auf 25er Runden
		///// DZ BERECHNUNG ////////

		pSensors->m_gear->calc();// alle 250 ms, also mit 4Hz den Gang berechnen

		pAktors->m_stepper->go_to(exact/10,0);

	} else if(now-previous_time>1000){
		rounded=0;
		exact=0;
		// zeiger
		pAktors->m_stepper->go_to(exact/10,0);
		previous_time=now;
	};
	if(DEMO_MODE){
		if(differ>250){
			previous_time=now;
			rounded=((millis()/300)%210)*70;
			exact=rounded;
			pSensors->m_gear->calc();
			// zeiger
			// 2 => 2*880=> 2k stepper
			pAktors->m_stepper->go_to(exact/11,0);
		}
	};
	/*///////////////// DIMMEN ABHÄNGIG VON DER DREHZAHL ////////////////
	 *  wenn wir über 12k sind und die außen LED noch nicht rot sind ..
	 *  1) es ist einfach nur blau 0,0,255
	 *  2) 0!=155 && 0!=255 && dimm_available()  ==> TRUE, Starte Dimm
	 *  3) 0..155!=155 && 0..255!=255 && dimm_available() => FALSE
	 *  3.1) 0..255!=255 && dimm_available() => FALSE  allein durch dimm_available()
	 *  wiederhole das bis dimm_available() wieder true wird
	 *  4) 155!=255 && 155!=155 && dimm_available()  ==> FALSE
	 *  5) 155!=255 && dimm_available() ==> TRUE
	 *  5.1) 155..255!=155 && 155..255!=255 && dimm_available() => FALSE allein durch dimm_available()
	 *  wiederhole das bis dimm_available() wieder true wird
	 */


	if(exact>blitz_dz && !hme_light_active && blitz_en){
		if(pAktors->dimm_available()){
			if(pAktors->RGB.outer.r.actual==pAktors->out_base_color.r.actual){
				// gucken ob der dimm Vorgang noch nicht gestartet wurde
				pAktors->dimm_rgb_to(5,5,5,15,0); // 25*10ms = 250 ms
			} else if(pAktors->RGB.outer.r.actual==5){
				pAktors->dimm_rgb_to(pAktors->dz_flasher.r.actual,pAktors->dz_flasher.g.actual,pAktors->dz_flasher.b.actual,15,0); // 25*10ms = 250 ms
				hme_light_active=true;
			};
		};
	// wenn wir unter 12k sind und die außen LED noch nicht ganz blau sind
	} else if(exact<=blitz_dz && hme_light_active && blitz_en) {
		if(pAktors->dimm_available()){
			if(pAktors->RGB.outer.r.actual==pAktors->dz_flasher.r.actual){
				pAktors->dimm_rgb_to(5,5,5,15,0); // 25*10ms = 250 ms
			} else if(pAktors->RGB.outer.r.actual==5){
				pAktors->dimm_rgb_to(pAktors->out_base_color.r.actual,pAktors->out_base_color.g.actual,pAktors->out_base_color.b.actual,15,0); // 75*10ms = 750 ms
				hme_light_active=false;
			}
		}
	};
	/////////////////// DIMMEN ABHÄNGIG VON DER DREHZAHL /////////////////

};

void helper(){
	pSensors->m_dz->counter();
}


void speedo_dz::init() {
	attachInterrupt(0, helper, RISING ); // interrupt handler für signalwechsel 0=DigiPin 2
	previous_time=millis();
	rounded=0;                 // to show on display, rounded by 50
	exact=0;                 // real rotation speed
	peak_count=0;

	Serial.println("DZ init done");
	blitz_en=false;
	hme_light_active=false; // nicht das high motor engine light an
	Serial3.flush();
};
