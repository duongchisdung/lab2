const stateMap = {
  pending: { text: 'Đang chờ', className: 'pending' },
  confirmed: { text: 'Đã xác nhận', className: 'confirmed' },
  timeout: { text: 'Quá hạn', className: 'timeout' },
  denied: { text: 'Bị từ chối', className: 'denied' },
};

async function fetchJson(url, options = {}) {
  const res = await fetch(url, options);
  if (!res.ok) {
    throw new Error(`HTTP ${res.status}`);
  }
  return res.json();
}

function renderTelemetry(data) {
  const tempEl = document.getElementById('temp');
  const humEl = document.getElementById('hum');
  const rainEl = document.getElementById('rain');
  const updatedEl = document.getElementById('updated');
  const staleBanner = document.getElementById('staleBanner');

  tempEl.textContent = data.temp ?? '--';
  humEl.textContent = data.hum ?? '--';
  rainEl.textContent = data.rain ?? '--';
  updatedEl.textContent = data.received_at ? new Date(data.received_at * 1000).toLocaleString('vi-VN') : '--';

  if (data.stale) {
    staleBanner.style.display = 'block';
  } else {
    staleBanner.style.display = 'none';
  }
}

function renderCommandState(status, message) {
  const el = document.getElementById('commandState');
  const map = stateMap[status] || stateMap.pending;
  el.textContent = map.text;
  el.className = `badge ${map.className}`;
  const msgEl = document.getElementById('commandMessage');
  msgEl.textContent = message || 'Chưa có lệnh nào.';
}

async function loadTelemetry() {
  try {
    const data = await fetchJson('/api/latest');
    renderTelemetry(data);
  } catch (err) {
    console.error(err);
  }
}

async function loadCommands() {
  try {
    const data = await fetchJson('/api/commands?limit=5');
    const rows = data.commands || [];
    const tbody = document.getElementById('commandRows');
    tbody.innerHTML = '';

    rows.forEach((cmd) => {
      const tr = document.createElement('tr');
      const state = stateMap[cmd.status] || stateMap.pending;
      tr.innerHTML = `
        <td>${cmd.cmd_id || '--'}</td>
        <td>${cmd.action || '--'}</td>
        <td><span class="badge ${state.className}">${state.text}</span></td>
        <td>${cmd.reason || '—'}</td>
      `;
      tbody.appendChild(tr);
      if (cmd.status === 'pending' || cmd.status === 'timeout' || cmd.status === 'confirmed' || cmd.status === 'denied') {
        renderCommandState(cmd.status, cmd.reason || `Lệnh: ${cmd.action}`);
      }
    });
  } catch (err) {
    console.error(err);
  }
}

async function submitCommand() {
  const token = document.getElementById('token').value.trim();
  const action = document.getElementById('action').value;
  try {
    renderCommandState('pending', 'Đang gửi lệnh...');
    const res = await fetch('/api/command', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Authorization': `Bearer ${token}`,
      },
      body: JSON.stringify({ action, device_id: 'node01', token }),
    });
    const data = await res.json();
    if (!res.ok) {
      renderCommandState('denied', data.reason || 'Từ chối bởi API');
      return;
    }
    renderCommandState('pending', `${data.message} | cmd_id=${data.cmd_id}`);
  } catch (err) {
    renderCommandState('denied', 'Không thể gửi lệnh');
  }
}

document.getElementById('sendCommand').addEventListener('click', submitCommand);

setInterval(loadTelemetry, 3000);
setInterval(loadCommands, 3000);
loadTelemetry();
loadCommands();
