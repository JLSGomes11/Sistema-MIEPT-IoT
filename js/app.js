// Cria um cliente MQTT WebSocket para comunicação com o broker
const client = new Paho.MQTT.Client(
  "broker.emqx.io",
  8084,
  "web_" + Math.random(),
);

// ===== Tópicos (devem ser iguais aos do secrets.h do ESP32) =====
const TOPICO_SENSORES = "jlsgomes/sensores";
const TOPICO_STATUS = "jlsgomes/status";
const TOPICO_ALERTA = "jlsgomes/alerta";

// ===== Limites (mesmos valores do firmware) =====
const LIM_ATENCAO = 2.5;
const LIM_PERIGO = 4.0;

// ===== Configurações do painel =====
const MAX_PONTOS = 300; // leituras guardadas no gráfico
const MAX_EVENTOS = 30; // eventos guardados no registro
const TIMEOUT_SEM_DADOS = 5000; // ms sem mensagem = dispositivo offline
const CHAVE_HISTORICO = "ponte_historico";
const CHAVE_EVENTOS = "ponte_eventos";

const NIVEIS = {
  normal: { texto: "Normal", classe: "ok" },
  atencao: { texto: "Atenção", classe: "atencao" },
  perigo: { texto: "Risco", classe: "alerta" },
  falha_sensor: { texto: "Falha de sensor", classe: "falha" },
};

const BANNERS = {
  perigo: ["is-danger", "Alerta: Ponte com movimentação estranha!"],
  atencao: ["is-warning", "Atenção: inclinação acima do normal. Acompanhe a estrutura."],
  falha_sensor: ["is-warning", "Falha em um dos sensores. Cancela fechada por segurança."],
};

if ("Notification" in window && Notification.permission === "default") {
  Notification.requestPermission();
}

// ============================================================
// Armazenamento local (navegador)
// ============================================================
function lerStorage(chave, padrao) {
  try {
    const v = localStorage.getItem(chave);
    return v ? JSON.parse(v) : padrao;
  } catch {
    return padrao;
  }
}

function salvarStorage(chave, valor) {
  try {
    localStorage.setItem(chave, JSON.stringify(valor));
  } catch {
    /* sem armazenamento disponível: segue sem salvar */
  }
}

// ============================================================
// Estado
// ============================================================
let historico = lerStorage(CHAVE_HISTORICO, null);
if (
  !historico ||
  !Array.isArray(historico.t) ||
  !Array.isArray(historico.a1) ||
  !Array.isArray(historico.a2) ||
  historico.t.length !== historico.a1.length ||
  historico.t.length !== historico.a2.length
) {
  historico = { t: [], a1: [], a2: [] };
}

let eventos = lerStorage(CHAVE_EVENTOS, []);
if (!Array.isArray(eventos)) eventos = [];

let ultimoNivel = null;
let ultimaMensagem = 0;
let lwtOffline = false;
let estadoDispositivo = "aguardando";

// ============================================================
// Gráfico de histórico
// ============================================================
const limites = [
  { label: "Limite de risco (±4°)", valor: LIM_PERIGO, cor: "#e74c3c", dados: [] },
  { label: "_", valor: -LIM_PERIGO, cor: "#e74c3c", dados: [] },
  { label: "Limite de atenção (±2,5°)", valor: LIM_ATENCAO, cor: "#f39c12", dados: [] },
  { label: "_", valor: -LIM_ATENCAO, cor: "#f39c12", dados: [] },
];
limites.forEach((l) => {
  l.dados = historico.t.map(() => l.valor);
});

let grafico = null;
if (typeof Chart !== "undefined") {
  const linhaLimite = (l) => ({
    label: l.label,
    data: l.dados,
    borderColor: l.cor,
    borderDash: [6, 4],
    borderWidth: 1.5,
    pointRadius: 0,
  });

  grafico = new Chart(document.getElementById("grafico"), {
    type: "line",
    data: {
      labels: historico.t,
      datasets: [
        {
          label: "Pilar 1",
          data: historico.a1,
          borderColor: "#3498db",
          backgroundColor: "#3498db",
          borderWidth: 2,
          tension: 0.25,
          pointRadius: 0,
          spanGaps: false,
        },
        {
          label: "Pilar 3",
          data: historico.a2,
          borderColor: "#16a085",
          backgroundColor: "#16a085",
          borderWidth: 2,
          tension: 0.25,
          pointRadius: 0,
          spanGaps: false,
        },
        ...limites.map(linhaLimite),
      ],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      interaction: { mode: "index", intersect: false },
      scales: {
        y: {
          title: { display: true, text: "Inclinação (°)" },
          suggestedMin: -5,
          suggestedMax: 5,
        },
        x: { ticks: { maxTicksLimit: 8, maxRotation: 0 } },
      },
      plugins: {
        legend: { labels: { filter: (item) => item.text !== "_" } },
        tooltip: {
          filter: (item) => !item.dataset.label.startsWith("Limite") && item.dataset.label !== "_",
        },
      },
    },
  });
}

function adicionarPonto(a1, a2) {
  historico.t.push(new Date().toLocaleTimeString("pt-BR"));
  historico.a1.push(a1);
  historico.a2.push(a2);
  limites.forEach((l) => l.dados.push(l.valor));

  while (historico.t.length > MAX_PONTOS) {
    historico.t.shift();
    historico.a1.shift();
    historico.a2.shift();
    limites.forEach((l) => l.dados.shift());
  }

  salvarStorage(CHAVE_HISTORICO, historico);
  if (grafico) grafico.update("none");
}

document.getElementById("btn-limpar").addEventListener("click", () => {
  historico.t.length = 0;
  historico.a1.length = 0;
  historico.a2.length = 0;
  limites.forEach((l) => (l.dados.length = 0));
  salvarStorage(CHAVE_HISTORICO, historico);
  if (grafico) grafico.update("none");
});

// ============================================================
// Registro de eventos
// ============================================================
function renderizarEventos() {
  const ul = document.getElementById("lista-eventos");
  ul.innerHTML = "";

  if (eventos.length === 0) {
    const li = document.createElement("li");
    li.textContent = "Nenhum evento registrado ainda.";
    ul.appendChild(li);
    return;
  }

  eventos.forEach((e) => {
    const li = document.createElement("li");

    const quando = document.createElement("span");
    quando.className = "quando";
    quando.textContent = e.quando;

    const tag = document.createElement("span");
    tag.className = "tag is-light is-" + e.tipo;
    tag.textContent = e.texto;

    li.appendChild(quando);
    li.appendChild(tag);
    ul.appendChild(li);
  });
}

function registrarEvento(texto, tipo) {
  const agora = new Date();
  const quando = agora.toLocaleDateString("pt-BR") + " " + agora.toLocaleTimeString("pt-BR");
  eventos.unshift({ quando, texto, tipo });
  if (eventos.length > MAX_EVENTOS) eventos.length = MAX_EVENTOS;
  salvarStorage(CHAVE_EVENTOS, eventos);
  renderizarEventos();
}

renderizarEventos();

// ============================================================
// Tela: sensores
// ============================================================
function mostrarPilar(id, valor, ok) {
  const el = document.getElementById(id);
  if (!ok) {
    el.textContent = "Falha";
    el.className = "sensor-value falha";
    return;
  }
  const abs = Math.abs(valor);
  el.textContent = valor.toFixed(1) + " °";
  el.className =
    "sensor-value " + (abs > LIM_PERIGO ? "alerta" : abs > LIM_ATENCAO ? "atencao" : "ok");
}

function mostrarBanner(nivel) {
  const el = document.getElementById("alerta-movimentacao");
  const cfg = BANNERS[nivel];
  if (!cfg) {
    el.style.display = "none";
    return;
  }
  el.className = "notification has-text-centered mt-4 " + cfg[0];
  el.textContent = cfg[1];
  el.style.display = "block";
}

function tratarSensores(texto) {
  let data;
  try {
    data = JSON.parse(texto);
  } catch {
    return; // mensagem inválida: ignora
  }
  // Validação básica: ignora dados fora do formato esperado
  if (
    !data ||
    !Number.isFinite(data.angulo1) ||
    !Number.isFinite(data.angulo2) ||
    Math.abs(data.angulo1) > 180 ||
    Math.abs(data.angulo2) > 180
  ) {
    return;
  }

  ultimaMensagem = Date.now();
  lwtOffline = false;

  // Compatível com o firmware antigo (sem "nivel")
  const nivel = NIVEIS[data.nivel] ? data.nivel : data.risco ? "perigo" : "normal";
  const risco =
    typeof data.risco === "boolean" ? data.risco : nivel === "perigo" || nivel === "falha_sensor";
  const s1ok = data.sensor1 !== "falha";
  const s2ok = data.sensor2 !== "falha";

  mostrarPilar("pilar1", data.angulo1, s1ok);
  mostrarPilar("pilar2", data.angulo2, s2ok);

  const statusPonte = document.getElementById("status-ponte");
  statusPonte.textContent = NIVEIS[nivel].texto;
  statusPonte.className = "sensor-value " + NIVEIS[nivel].classe;
  mostrarBanner(nivel);

  // Botão só habilita quando a cancela está fechada por risco
  document.getElementById("btn-alerta").disabled = !risco;

  const statusCancela = document.getElementById("status-cancela");
  statusCancela.textContent = data.cancela === "fechada" ? "Fechada" : "Aberta";
  statusCancela.className =
    data.cancela === "fechada" ? "sensor-value alerta" : "sensor-value ok";

  // Registra mudanças de nível
  if (ultimoNivel !== null && nivel !== ultimoNivel) {
    const tipos = { normal: "success", atencao: "warning", perigo: "danger", falha_sensor: "dark" };
    registrarEvento("Nível mudou para: " + NIVEIS[nivel].texto, tipos[nivel]);
  }
  ultimoNivel = nivel;

  // Falha de sensor vira "buraco" no gráfico
  adicionarPonto(s1ok ? data.angulo1 : null, s2ok ? data.angulo2 : null);
}

// ============================================================
// Tela: status do dispositivo (heartbeat + Last Will)
// ============================================================
function tratarStatus(texto, retida) {
  let d;
  try {
    d = JSON.parse(texto);
  } catch {
    return;
  }
  if (!d) return;

  if (d.status === "offline") {
    lwtOffline = true;
  } else if (d.status === "online") {
    lwtOffline = false;
    if (!retida) {
      registrarEvento("Dispositivo conectou (último reinício: " + (d.reinicio || "?") + ")", "info");
    }
  }
}

function atualizarDispositivo() {
  let novo;
  if (!client.isConnected()) novo = "semconexao";
  else if (lwtOffline) novo = "offline";
  else if (!ultimaMensagem) novo = "aguardando";
  else novo = Date.now() - ultimaMensagem > TIMEOUT_SEM_DADOS ? "offline" : "online";

  const el = document.getElementById("status-dispositivo");
  if (novo === "online") {
    el.textContent = "Dispositivo: Online";
    el.className = "has-text-centered status-ok";
  } else if (novo === "offline") {
    const seg = ultimaMensagem ? Math.floor((Date.now() - ultimaMensagem) / 1000) : null;
    el.textContent = "Dispositivo: Offline" + (seg !== null ? " (sem dados há " + seg + "s)" : "");
    el.className = "has-text-centered status-erro";
  } else if (novo === "semconexao") {
    el.textContent = "Dispositivo: sem conexão com o broker";
    el.className = "has-text-centered status-erro";
  } else {
    el.textContent = "Dispositivo: aguardando dados...";
    el.className = "has-text-centered status-aviso";
  }

  // Valores antigos ficam apagados enquanto o dispositivo está offline
  document.getElementById("painel").classList.toggle("desatualizado", novo === "offline");

  if (novo !== estadoDispositivo) {
    if (novo === "offline") {
      registrarEvento("Dispositivo sem comunicação", "danger");
    } else if (novo === "online" && estadoDispositivo === "offline") {
      registrarEvento("Dispositivo voltou a comunicar", "success");
    }
    estadoDispositivo = novo;
  }
}
setInterval(atualizarDispositivo, 1000);

// ============================================================
// MQTT
// ============================================================
client.onMessageArrived = function (message) {
  // Alerta manual (botão)
  if (message.destinationName === TOPICO_ALERTA) {
    if ("Notification" in window && Notification.permission === "granted") {
      new Notification("Alerta: Ponte Interditada", {
        body: "A ponte 1 foi marcada como interditada. Evite a rota.",
      });
    }
    return;
  }

  // Status do dispositivo (online/offline)
  if (message.destinationName === TOPICO_STATUS) {
    tratarStatus(message.payloadString, message.retained);
    return;
  }

  // Dados dos sensores (automático)
  tratarSensores(message.payloadString);
};

client.onConnectionLost = function () {
  const status = document.getElementById("status");
  status.textContent = "Reconectando...";
  status.className = "has-text-centered status-erro";
  setTimeout(conectar, 2000);
};

function conectar() {
  client.connect({
    useSSL: true,
    onSuccess: () => {
      const status = document.getElementById("status");
      status.textContent = "Conectado";
      status.className = "has-text-centered status-ok";

      ultimaMensagem = 0; // espera dados novos antes de dizer "online"
      client.subscribe(TOPICO_SENSORES);
      client.subscribe(TOPICO_STATUS);
      client.subscribe(TOPICO_ALERTA);
    },
    onFailure: () => {
      const status = document.getElementById("status");
      status.textContent = "Reconectando...";
      status.className = "has-text-centered status-erro";
      setTimeout(conectar, 2000);
    },
  });
}
conectar();

document.getElementById("btn-alerta").addEventListener("click", () => {
  if (!("Notification" in window)) {
    alert(
      "Seu navegador não suporta notificações. O alerta será enviado, mas você não vai vê-lo aqui.",
    );
  } else if (Notification.permission === "denied") {
    alert(
      "As notificações estão bloqueadas no seu navegador. Ative-as nas configurações do site para receber este alerta.",
    );
  } else if (Notification.permission === "default") {
    alert("Ative as notificações do navegador para receber este alerta.");
    Notification.requestPermission();
  }

  const mensagem = new Paho.MQTT.Message("interditada");
  mensagem.destinationName = TOPICO_ALERTA;
  client.send(mensagem);
});
