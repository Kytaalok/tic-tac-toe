// server.js

const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const net = require('net');

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

// Настройки подключения к C-серверу
const C_SERVER_HOST = '127.0.0.1'; // Замените на IP вашего C-сервера, если необходимо
const C_SERVER_PORT = 8080;        // Порт вашего C-сервера

// Подключение к C-серверу
const cServerClient = new net.Socket();

cServerClient.connect(C_SERVER_PORT, C_SERVER_HOST, () => {
    console.log('Подключено к C-серверу');
});

// Буфер для разделения сообщений
let buffer = '';

//Обработка данных от C-сервера
cServerClient.on('data', (data) => {
    buffer += data.toString();
    let boundary = buffer.indexOf('\n');
    wss.clients.forEach((client) => {
        if (client.readyState === WebSocket.OPEN) {
            //client.send(data.toString());
        }
    });

});

// Обработка закрытия соединения с C-сервером
cServerClient.on('close', () => {
    console.log('Соединение с C-сервером закрыто');
});

// Обработка ошибок при подключении к C-серверу
cServerClient.on('error', (err) => {
    console.error('Ошибка подключения к C-серверу:', err.message);
});

const clientSocketMap = new Map();
//const { v4: uuidv4 } = require('uuid');

// Обработка WebSocket подключений
wss.on('connection', (ws) => {
    //ws.id = uuidv4();

    const clientSocket = new net.Socket(); // Уникальный сокет для каждого клиента

    clientSocket.connect(C_SERVER_PORT, C_SERVER_HOST, () => {
        console.log(`Подключен новый клиентский сокет к C-серверу для ID=${ws.clientSocket}`);
        //clientSocketMap.set(ws, clientSocket); 
    });

    // Обработка данных от C-сервера для данного клиента
    clientSocket.on('data', (data) => {
        const message = data.toString().trim(); 
        ws.send(data.toString()); // Только этому клиенту
        console.log('Сообщение от C-сервера:', message);

        if (message.startsWith("UPDATE_BOARD")) {
            ws.send(message); // Отправляем только текущему клиенту  
        } else if (message.startsWith("OK GameJoined")) {
            ws.send(message); // Пересылаем ID игры подключившемуся клиенту
        } else if (message.startsWith("NEXT_PLAYER")){
            ws.send(message); // Все остальные сообщения
        } else if (message.startsWith("CHAT")){
            //ws.send(message); // Все остальные сообщения
        } else if (message.startsWith("GAME_OVER")){
            //ws.send(message); // Все остальные сообщения
        } else if (message.startsWith("OK Registered")){
            //ws.send(message); // Все остальные сообщения
        }else if (message.startsWith("START_GAME")){
            //ws.send(message); // Все остальные сообщения
        }else {
            ws.send(message); // Все остальные сообщения
        }
    });

    // Обработка сообщений от WebSocket клиента
    ws.on('message', (message) => {
        console.log(`Получено от клиента: ${message}`);
        clientSocket.write(message + '\n'); // Отправка на C-сервер
    });

    function requestGameList() {
        // отправляем LIST_GAMES на сервер
        ws.send("LIST_GAMES");
    }
    
    // например, каждые 5 секунд
    // setInterval(() => {
    //     // Если мы не находимся в игре, спрашиваем список
    //     if (!currentGameId) {
    //         requestGameList();
    //     }
    // }, 5000);
    

    // Обработка закрытия соединений
    ws.on('close', () => {
        console.log(`WebSocket клиент ID=${ws.id} отключен`);
        clientSocket.end();
    });
});



// Обслуживание статических файлов (веб-интерфейс)
app.use(express.static('public'));

// Запуск сервера на всех интерфейсах
const PORT = 3000;
const HOST = '0.0.0.0'; // Прослушивание на всех интерфейсах
server.listen(PORT, HOST, () => {
    console.log(`Node.js сервер слушает на ${HOST}:${PORT}`);
});
