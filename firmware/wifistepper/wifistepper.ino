#include <FS.h>
#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include <PubSubClient.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>

#include "powerstep01.h"
#include "wifistepper.h"

#define DEFAULT_APSSID  {'w','s','x','1','0','0','-','a','q',0}

#define TYPE_HTML     "text/html"
#define TYPE_CSS      "text/css"
#define TYPE_JS       "application/javascript"
#define TYPE_PNG      "image/png"

volatile id_t _id = ID_START;

ESP8266WebServer server(PORT_HTTP);
WebSocketsServer websocket(PORT_HTTPWS);

WiFiClient * mqtt_conn = NULL;
PubSubClient * mqtt_client = NULL;

StaticJsonBuffer<2048> jsonbuf;
StaticJsonBuffer<1024> configbuf;

volatile bool flag_reboot = false;
volatile bool flag_wifiled = false;
volatile bool flag_cmderror = false;

config_t config = {
  .wifi = {
    .mode = M_ACCESSPOINT,
    .accesspoint = {
      .ssid = DEFAULT_APSSID,
      .password = {0},
      .encryption = false,
      .channel = 1,
      .hidden = false
    },
    .station = {
      .ssid = {0},
      .password = {0},
      .encryption = false,
      .forceip = {0},
      .forcesubnet = {0},
      .forcegateway = {0},
      .revertap = true
    }
  },
  .service = {
    .http = {
      .enabled = true,
    },
    .mdns = {
      .enabled = true,
      .hostname = {'w','s','x','1','0','0',0}
    },
    .auth = {
      .enabled = false,
      .username = {0},
      .password = {0}
    },
    .ota = {
      .enabled = true,
      .password = {0}
    },
    .daisy = {
      .enabled = true,
      .master = true,
      .baudrate = 1000000
    },
    .mqtt = {
      .enabled = false,
      .server = {'i','o','.','a','d','a','f','r','u','i','t','.','c','o','m',0},
      .port = 1883,
      .username = {'a','k','l','o','f','a','s',0},
      .key = {'b','b','7','3','3','c','b','e','4','a','9','b','4','7','5','2','a','c','0','d','a','a','b','b','b','e','4','2','f','b','5','7',0},
      .state_topic = {'a','k','l','o','f','a','s','/','f','e','e','d','s','/','s','t','a','t','e',0},
      .state_publish_period = 5.0,
      .command_topic = {'a','k','l','o','f','a','s','/','f','e','e','d','s','/','c','o','m','m','a','n','d',0}
    }
  },
  .io = {
    .wifiled = {
      .usercontrol = false,
      .is_output = true
    }
  },
  .motor = {
    .mode = MODE_CURRENT,
    .stepsize = STEP_16,
    .ocd = 500.0,
    .ocdshutdown = true,
    .maxspeed = 10000.0,
    .minspeed = 0.0,
    .accel = 1000.0,
    .decel = 1000.0,
    .kthold = 0.15,
    .ktrun = 0.15,
    .ktaccel = 0.15,
    .ktdecel = 0.15,
    .fsspeed = 2000.0,
    .fsboost = false,
    .cm = {
      .switchperiod = 44,
      .predict = true,
      .minon = 21,
      .minoff = 21,
      .fastoff = 4,
      .faststep = 20
    },
    .vm = {
      .pwmfreq = 23.4,
      .stall = 531.25,
      .bemf_slopel = 0.0375,
      .bemf_speedco = 61.5072,
      .bemf_slopehacc = 0.0615,
      .bemf_slopehdec = 0.0615
    },
    .reverse = false
  }
};

state_t state = { 0 };
sketch_t sketch = { 0 };

daisy_slave_t * daisy_slave = NULL;

id_t nextid() { return _id++; }
id_t currentid() { return _id; }

unsigned long timesince(unsigned long t1, unsigned long t2) {
  return (t1 <= t2)? (t2 - t1) : (ULONG_MAX - t1 + t2);
}

void seterror(uint8_t subsystem, id_t onid, int type) {
  if (!state.error.errored) {
    state.error.when = millis();
    state.error.subsystem = subsystem;
    state.error.id = onid;
    state.error.type = type;
  }
}

void wificfg_read(wifi_config * cfg) {
  File fp = SPIFFS.open(FNAME_WIFICFG, "r");
  if (fp) {
    size_t size = fp.size();
    std::unique_ptr<char[]> buf(new char[size]);
    fp.readBytes(buf.get(), size);
    JsonObject& root = jsonbuf.parseObject(buf.get());
    if (root.containsKey("mode"))           cfg->mode = parse_wifimode(root["mode"].as<char *>());
    if (root.containsKey("accesspoint_ssid")) strlcpy(cfg->accesspoint.ssid, root["ap_ssid"].as<char *>(), LEN_SSID);
    if (root.containsKey("accesspoint_password")) strlcpy(cfg->accesspoint.password, root["accesspoint_password"].as<char *>(), LEN_PASSWORD);
    if (root.containsKey("accesspoint_encryption")) cfg->accesspoint.encryption = root["accesspoint_encryption"].as<bool>();
    if (root.containsKey("accesspoint_channel")) cfg->accesspoint.channel = root["accesspoint_channel"].as<int>();
    if (root.containsKey("accesspoint_hidden")) cfg->accesspoint.hidden = root["accesspoint_hidden"].as<bool>();
    if (root.containsKey("station_ssid"))   strlcpy(cfg->station.ssid, root["station_ssid"].as<char *>(), LEN_SSID);
    if (root.containsKey("station_password")) strlcpy(cfg->station.password, root["station_password"].as<char *>(), LEN_PASSWORD);
    if (root.containsKey("station_encryption")) cfg->station.encryption = root["station_encryption"].as<bool>();
    if (root.containsKey("station_forceip")) strlcpy(cfg->station.forceip, root["station_forceip"].as<char *>(), LEN_IP);
    if (root.containsKey("station_forcesubnet")) strlcpy(cfg->station.forcesubnet, root["station_forcesubnet"].as<char *>(), LEN_IP);
    if (root.containsKey("station_forcegateway")) strlcpy(cfg->station.forcegateway, root["station_forcegateway"].as<char *>(), LEN_IP);
    if (root.containsKey("station_revertap")) cfg->station.revertap = root["station_revertap"].as<bool>();
    jsonbuf.clear();
    fp.close();
  }
}

void wificfg_write(wifi_config * const cfg) {
  JsonObject& root = jsonbuf.createObject();
  root["mode"] = json_serialize(cfg->mode);
  root["accesspoint_ssid"] = cfg->accesspoint.ssid;
  root["accesspoint_password"] = cfg->accesspoint.password;
  root["accesspoint_encryption"] = cfg->accesspoint.encryption;
  root["accesspoint_channel"] = cfg->accesspoint.channel;
  root["accesspoint_hidden"] = cfg->accesspoint.hidden;
  root["station_ssid"] = cfg->station.ssid;
  root["station_password"] = cfg->station.password;
  root["station_encryption"] = cfg->station.encryption;
  root["station_forceip"] = cfg->station.forceip;
  root["station_forcesubnet"] = cfg->station.forcesubnet;
  root["station_forcegateway"] = cfg->station.forcegateway;
  root["station_revertap"] = cfg->station.revertap;
  JsonVariant v = root;
  File fp = SPIFFS.open(FNAME_WIFICFG, "w");
  root.printTo(fp);
  fp.close();
  jsonbuf.clear();
}

void servicecfg_read(service_config * cfg) {
  File fp = SPIFFS.open(FNAME_SERVICECFG, "r");
  if (fp) {
    size_t size = fp.size();
    std::unique_ptr<char[]> buf(new char[size]);
    fp.readBytes(buf.get(), size);
    JsonObject& root = jsonbuf.parseObject(buf.get());
    if (root.containsKey("http_enabled"))   cfg->http.enabled = root["http_enabled"].as<bool>();
    if (root.containsKey("mdns_enabled"))   cfg->mdns.enabled = root["mdns_enabled"].as<bool>();
    if (root.containsKey("mdns_hostname"))  strlcpy(cfg->mdns.hostname, root["mdns_hostname"].as<char *>(), LEN_HOSTNAME);
    if (root.containsKey("auth_enabled"))   cfg->auth.enabled = root["auth_enabled"].as<bool>();
    if (root.containsKey("auth_username"))  strlcpy(cfg->auth.username, root["auth_username"].as<char *>(), LEN_USERNAME);
    if (root.containsKey("auth_password"))  strlcpy(cfg->auth.password, root["auth_password"].as<char *>(), LEN_PASSWORD);
    if (root.containsKey("ota_enabled"))    cfg->ota.enabled = root["ota_enabled"].as<bool>();
    if (root.containsKey("ota_password"))   strlcpy(cfg->ota.password, root["ota_password"].as<char *>(), LEN_PASSWORD);
    if (root.containsKey("daisy_enabled"))  cfg->daisy.enabled = root["daisy_enabled"].as<bool>();
    if (root.containsKey("daisy_master"))   cfg->daisy.master = root["daisy_master"].as<bool>();
    if (root.containsKey("daisy_baudrate")) cfg->daisy.baudrate = root["daisy_baudrate"].as<int>();
    if (root.containsKey("mqtt_enabled"))   cfg->mqtt.enabled = root["mqtt_enabled"].as<bool>();
    if (root.containsKey("mqtt_server"))    strlcpy(cfg->mqtt.server, root["mqtt_server"].as<char *>(), LEN_URL);
    if (root.containsKey("mqtt_port"))      cfg->mqtt.port = root["mqtt_port"].as<int>();
    if (root.containsKey("mqtt_username"))  strlcpy(cfg->mqtt.username, root["mqtt_username"].as<char *>(), LEN_USERNAME);
    if (root.containsKey("mqtt_key"))       strlcpy(cfg->mqtt.key, root["mqtt_key"].as<char *>(), LEN_PASSWORD);
    if (root.containsKey("mqtt_state_topic")) strlcpy(cfg->mqtt.state_topic, root["mqtt_state_topic"].as<char *>(), LEN_URL);
    if (root.containsKey("mqtt_state_publish_period")) cfg->mqtt.state_publish_period = root["mqtt_state_publish_period"].as<float>();
    if (root.containsKey("mqtt_command_topic")) strlcpy(cfg->mqtt.command_topic, root["mqtt_command_topic"].as<char *>(), LEN_URL);
    jsonbuf.clear();
    fp.close();
  }
}

void servicecfg_write(service_config * const cfg) {
  JsonObject& root = jsonbuf.createObject();
  root["http_enabled"] = cfg->http.enabled;
  root["mdns_enabled"] = cfg->mdns.enabled;
  root["mdns_hostname"] = cfg->mdns.hostname;
  root["auth_enabled"] = cfg->auth.enabled;
  root["auth_username"] = cfg->auth.username;
  root["auth_password"] = cfg->auth.password;
  root["ota_enabled"] = cfg->ota.enabled;
  root["ota_password"] = cfg->ota.password;
  root["daisy_enabled"] = cfg->daisy.enabled;
  root["daisy_master"] = cfg->daisy.master;
  root["daisy_baudrate"] = cfg->daisy.baudrate;
  root["mqtt_enabled"] = cfg->mqtt.enabled;
  root["mqtt_server"] = cfg->mqtt.server;
  root["mqtt_port"] = cfg->mqtt.port;
  root["mqtt_username"] = cfg->mqtt.username;
  root["mqtt_key"] = cfg->mqtt.key;
  root["mqtt_state_topic"] = cfg->mqtt.state_topic;
  root["mqtt_state_publish_period"] = cfg->mqtt.state_publish_period;
  root["mqtt_command_topic"] = cfg->mqtt.command_topic;
  JsonVariant v = root;
  File fp = SPIFFS.open(FNAME_SERVICECFG, "w");
  root.printTo(fp);
  fp.close();
  jsonbuf.clear();
}

void motorcfg_pull(motor_config * cfg) {
  cfg->mode = ps_getmode();
  cfg->stepsize = ps_getstepsize();
  ps_ocd ocd = ps_getocd();
  cfg->ocd = ocd.millivolts;
  cfg->ocdshutdown = ocd.shutdown;
  cfg->maxspeed = ps_getmaxspeed();
  ps_minspeed minspeed = ps_getminspeed();
  cfg->minspeed = minspeed.steps_per_sec;
  cfg->accel = ps_getaccel();
  cfg->decel = ps_getdecel();
  ps_ktvals ktvals = ps_getktvals();
  cfg->kthold = ktvals.hold;
  cfg->ktrun = ktvals.run;
  cfg->ktaccel = ktvals.accel;
  cfg->ktdecel = ktvals.decel;
  ps_fullstepspeed fullstepspeed = ps_getfullstepspeed();
  cfg->fsspeed = fullstepspeed.steps_per_sec;
  cfg->fsboost = fullstepspeed.boost_mode;
  if (cfg->mode == MODE_CURRENT) {
    cfg->cm.switchperiod = ps_cm_getswitchperiod();
    cfg->cm.predict = ps_cm_getpredict();
    ps_cm_ctrltimes ctrltimes = ps_cm_getctrltimes();
    cfg->cm.minon = ctrltimes.min_on_us;
    cfg->cm.minoff = ctrltimes.min_off_us;
    cfg->cm.fastoff = ctrltimes.fast_off_us;
    cfg->cm.faststep = ctrltimes.fast_step_us;
  } else {
    ps_vm_pwmfreq pwmfreq = ps_vm_getpwmfreq();
    cfg->vm.pwmfreq = ps_vm_coeffs2pwmfreq(MOTOR_CLOCK, &pwmfreq) / 1000.0;
    cfg->vm.stall = ps_vm_getstall();
    ps_vm_bemf bemf = ps_vm_getbemf();
    cfg->vm.bemf_slopel = bemf.slopel;
    cfg->vm.bemf_speedco = bemf.speedco;
    cfg->vm.bemf_slopehacc = bemf.slopehacc;
    cfg->vm.bemf_slopehdec = bemf.slopehdec;
  }
}

void motorcfg_push(motor_config * cfg) {
  ps_setsync(SYNC_BUSY);
  ps_setmode(cfg->mode);
  ps_setstepsize(cfg->stepsize);
  ps_setmaxspeed(cfg->maxspeed);
  ps_setminspeed(cfg->minspeed, true);
  ps_setaccel(cfg->accel);
  ps_setdecel(cfg->decel);
  ps_setfullstepspeed(cfg->fsspeed, cfg->fsboost);
  
  ps_setslewrate(SR_520);

  ps_setocd(cfg->ocd, cfg->ocdshutdown);
  if (cfg->mode == MODE_CURRENT) {
    ps_cm_setswitchperiod(cfg->cm.switchperiod);
    ps_cm_setpredict(cfg->cm.predict);
    ps_cm_setctrltimes(cfg->cm.minon, cfg->cm.minoff, cfg->cm.fastoff, cfg->cm.faststep);
  } else {
    ps_vm_pwmfreq pwmfreq = ps_vm_pwmfreq2coeffs(MOTOR_CLOCK, cfg->vm.pwmfreq * 1000.0);
    ps_vm_setpwmfreq(&pwmfreq);
    ps_vm_setstall(cfg->vm.stall);
    ps_vm_setbemf(cfg->vm.bemf_slopel, cfg->vm.bemf_speedco, cfg->vm.bemf_slopehacc, cfg->vm.bemf_slopehdec);
  }
  
  ps_setvoltcomp(false);
  ps_setswmode(SW_USER);
  ps_setclocksel(MOTOR_CLOCK);

  ps_setktvals(cfg->kthold, cfg->ktrun, cfg->ktaccel, cfg->ktdecel);
  ps_setalarmconfig(true, true, true, true);

  // Clear errors at end of push
  ps_getstatus(true);
}

void motorcfg_write(motor_config * cfg) {
  JsonObject& root = jsonbuf.createObject();
  root["mode"] = json_serialize(cfg->mode);
  root["stepsize"] = json_serialize(cfg->stepsize);
  root["ocd"] = cfg->ocd;
  root["ocdshutdown"] = cfg->ocdshutdown;
  root["maxspeed"] = cfg->maxspeed;
  root["minspeed"] = cfg->minspeed;
  root["accel"] = cfg->accel;
  root["decel"] = cfg->decel;
  root["kthold"] = cfg->kthold;
  root["ktrun"] = cfg->ktrun;
  root["ktaccel"] = cfg->ktaccel;
  root["ktdecel"] = cfg->ktdecel;
  root["fsspeed"] = cfg->fsspeed;
  root["fsboost"] = cfg->fsboost;
  root["cm_switchperiod"] = cfg->cm.switchperiod;
  root["cm_predict"] = cfg->cm.predict;
  root["cm_minon"] = cfg->cm.minon;
  root["cm_minoff"] = cfg->cm.minoff;
  root["cm_fastoff"] = cfg->cm.fastoff;
  root["cm_faststep"] = cfg->cm.faststep;
  root["vm_pwmfreq"] = cfg->vm.pwmfreq;
  root["vm_stall"] = cfg->vm.stall;
  root["vm_bemf_slopel"] = cfg->vm.bemf_slopel;
  root["vm_bemf_speedco"] = cfg->vm.bemf_speedco;
  root["vm_bemf_slopehacc"] = cfg->vm.bemf_slopehacc;
  root["vm_bemf_slopehdec"] = cfg->vm.bemf_slopehdec;
  root["reverse"] = cfg->reverse;
  JsonVariant v = root;
  File fp = SPIFFS.open(FNAME_MOTORCFG, "w");
  root.printTo(fp);
  fp.close();
  jsonbuf.clear();
}



void static_serve(String contenttype, String path) {
  File fp = SPIFFS.open(path, "r");
  if (fp) {
    server.streamFile(fp, contenttype);
    fp.close();
  } else {
    server.send(404, "text/plain", "File not found.");
  }
}

void static_init() {
  server.on("/", [](){
    String path;
    char def_apssid[] = DEFAULT_APSSID;
    if (config.wifi.mode == M_ACCESSPOINT && strcmp(config.wifi.accesspoint.ssid, def_apssid) == 0) {
      path = "/settings";
    } else {
      path = "/quickstart";
    }
    server.sendHeader("Location", path);
    server.send(302, "text/plain", String("Redirect to ") + path);
  });
  
  server.on("/quickstart", [](){ static_serve(TYPE_HTML, "/quickstart.html.gz"); });
  server.on("/dashboard", [](){ static_serve(TYPE_HTML, "/dashboard.html.gz"); });
  server.on("/settings", [](){ static_serve(TYPE_HTML, "/settings.html.gz"); });
  server.on("/documentation", [](){ static_serve(TYPE_HTML, "/documentation.html.gz"); });
  server.on("/troubleshoot", [](){ static_serve(TYPE_HTML, "/troubleshoot.html.gz"); });
  server.on("/about", [](){ static_serve(TYPE_HTML, "/about.html.gz"); });
  
  server.on("/js/axios.min.js", [](){ static_serve(TYPE_JS, "/js/axios.min.js.gz"); });
  server.on("/js/clipboard.min.js", [](){ static_serve(TYPE_JS, "/js/clipboard.min.js.gz"); });
  server.on("/js/prism.min.js", [](){ static_serve(TYPE_JS, "/js/prism.min.js.gz"); });
  server.on("/js/vue.min.js", [](){ static_serve(TYPE_JS, "/js/vue.min.js.gz"); });
  server.on("/js/vue-cookies.min.js", [](){ static_serve(TYPE_JS, "/js/vue-cookies.min.js.gz"); });
}



void setup() {
  // Initialize early subsystems
  {
    cmd_init();
    daisy_init();
  }

  // Initialize FS
  {
    SPIFFS.begin();
    
    // Check reset
    pinMode(RESET_PIN, INPUT_PULLUP);
    while (!digitalRead(RESET_PIN) && millis() < RESET_TIMEOUT) {
      delay(1);
    }
    if (millis() >= RESET_TIMEOUT) {
      // Reset chosen
      Serial.println("Configuration Reset");
      SPIFFS.remove(FNAME_WIFICFG);
      SPIFFS.remove(FNAME_SERVICECFG);
      SPIFFS.remove(FNAME_MOTORCFG);

      // Wait for reset to depress
      while (!digitalRead(RESET_PIN)) {
        delay(100);
      }
    }
  }

  // Read wifi configuration
  {
    wificfg_read(&config.wifi);
  }

  // Read browser configuration
  {
    servicecfg_read(&config.service);
  }

  // Wifi connection
  {

    // TODO - handle IO config
    pinMode(WIFILED_PIN, OUTPUT);
    digitalWrite(WIFILED_PIN, HIGH);
    
    if (config.service.mdns.enabled) {
      WiFi.hostname(config.service.mdns.hostname);
    }

    bool revertap = false;
    bool wifioff = false;
    if (config.wifi.mode == M_STATION) {
      Serial.println(config.wifi.station.ssid);
      Serial.println(config.wifi.station.password);
      WiFi.mode(WIFI_STA);
      if (config.wifi.station.forceip[0] != 0 && config.wifi.station.forcesubnet[0] != 0 && config.wifi.station.forcegateway[0] != 0) {
        IPAddress addr, subnet, gateway;
        if (addr.fromString(config.wifi.station.forceip) && subnet.fromString(config.wifi.station.forcesubnet) && gateway.fromString(config.wifi.station.forcegateway)) {
          WiFi.config(addr, subnet, gateway);
        }
      }
      WiFi.begin(config.wifi.station.ssid, config.wifi.station.password);
      for (int i=0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
      }

      if (WiFi.status() != WL_CONNECTED) {
        if (config.wifi.station.revertap) revertap = true;
        else wifioff = true;
      } else {
        state.wifi.mode = M_STATION;
        strlcpy(state.wifi.ip, WiFi.localIP().toString().c_str(), LEN_IP);
        digitalWrite(WIFILED_PIN, LOW);
      }
    }

    if (config.wifi.mode == M_ACCESSPOINT || revertap) {
      Serial.println(config.wifi.accesspoint.ssid);
      Serial.println(config.wifi.accesspoint.password);
      WiFi.mode(WIFI_AP);
      WiFi.softAP(config.wifi.accesspoint.ssid, config.wifi.accesspoint.password, config.wifi.accesspoint.channel, config.wifi.accesspoint.hidden);
      state.wifi.mode = M_ACCESSPOINT;
      strlcpy(state.wifi.ip, WiFi.softAPIP().toString().c_str(), LEN_IP);
    }

    if (config.wifi.mode == M_OFF || wifioff) {
      WiFi.mode(WIFI_OFF);
      state.wifi.mode = M_OFF;
      state.wifi.ip[0] = 0;
    }
  }

  // Initialize web services
  {
    if (config.service.mdns.enabled) {
      MDNS.begin(config.service.mdns.hostname);
    }

    if (config.service.ota.enabled) {
      ArduinoOTA.setHostname(config.service.mdns.hostname);
      if (config.service.ota.password[0] != 0)
        ArduinoOTA.setPassword(config.service.ota.password);
      ArduinoOTA.begin();
    }

    if (config.service.http.enabled) {
      json_init();
      static_init();
      server.begin();
      websocket_init();
    }

    if (config.service.mqtt.enabled) {
      mqtt_init();
    }
  }

  // Initialize SPI and Stepper Motor config
  {
    ps_spiinit();

    // Read motor config
    File fp = SPIFFS.open(FNAME_MOTORCFG, "r");
    if (fp) {
      size_t size = fp.size();
      std::unique_ptr<char[]> buf(new char[size]);
      fp.readBytes(buf.get(), size);
      cmd_setconfig(nextid(), buf.get());
      fp.close();
      
    } else {
      // No motor config, send default
      cmd_setconfig(nextid(), "");
    }
  }
}

#define HANDLE_CMDS()     ({ daisy_loop(now); cmd_loop(now); })

//static volatile unsigned long last_statepoll = 0;
void loop() {
  unsigned long now = millis();

  HANDLE_CMDS();

  if (config.service.http.enabled) {
    websocket.loop();
    server.handleClient();
  }

  HANDLE_CMDS();

  if (config.service.mqtt.enabled) {
    mqtt_loop(now);
  }

  HANDLE_CMDS();

  if (config.service.ota.enabled) {
    ArduinoOTA.handle();
  }

  // Reboot if requested
  if (flag_reboot) {
    ESP.restart();
  }

  // Handle wifi LED blinks
  if (!config.io.wifiled.usercontrol) {
    if (config.wifi.mode == M_ACCESSPOINT) {
      bool isoff = ((now / 200) % 20) == 1;
      digitalWrite(WIFILED_PIN, isoff? HIGH : LOW);
    }
  }

  cmd_update(now);
  daisy_update(now);
}

