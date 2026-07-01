const { Client } = require('ssh2');
const conn = new Client();
conn.on('ready', () => {
  console.log('Connecting to install sqlite3...');
  conn.exec('cd /root/owibot && npm install sqlite3', (err, stream) => {
    if (err) throw err;
    stream.on('data', d => process.stdout.write(d));
    stream.stderr.on('data', d => process.stderr.write(d));
    stream.on('close', () => {
      console.log('\nsqlite3 installed! Now running upload_vps.js...');
      conn.end();
      require('child_process').execSync('node upload_vps.js', { stdio: 'inherit' });
    });
  });
}).connect({ host: '212.2.253.247', port: 22, username: 'root', password: 'cAh2TrVUlG' });
