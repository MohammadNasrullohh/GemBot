require('dotenv').config();
const fs = require('fs');
const { Client } = require('ssh2');
const conn = new Client();
const host = process.env.VPS_HOST || '212.2.253.247';
const username = process.env.VPS_USER || 'root';
const password = process.env.VPS_PASSWORD;

if (!password) {
  console.error('Set VPS_PASSWORD di .env lokal dulu.');
  process.exit(1);
}

conn.on('ready', () => {
  conn.sftp((err, sftp) => {
    if (err) throw err;
    sftp.fastPut('vps_server_local.js', '/root/owibot/vps_server.js', (err) => {
      if (err) throw err;
      console.log('Uploaded server');
      conn.exec('cd /root/owibot && node --check vps_server.js && pm2 restart owi', (err, stream) => {
         if (err) throw err;
         stream.on('data', d => console.log(d.toString()));
         stream.stderr.on('data', d => console.error(d.toString()));
         stream.on('close', () => conn.end());
      });
    });
  });
}).connect({ host, port: 22, username, password });
