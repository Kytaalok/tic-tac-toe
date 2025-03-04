# Tic-Tac-Toe Game

Этот проект представляет собой многопользовательскую онлайн-игру "Крестики-Нолики" с серверной частью на C и Node.js, а также веб-интерфейсом для взаимодействия с игроками.

## 📌 Функционал
- Регистрация и авторизация пользователей.
- Создание и подключение к играм.
- Гибкий выбор размера игрового поля.
- Обновление состояния игры в реальном времени через WebSocket.
- Чат между игроками.
- Поддержка нескольких активных игр.

## 📁 Структура проекта
- `server.c` — сервер на C, отвечающий за обработку логики игры.
- `server.js` — WebSocket сервер на Node.js, взаимодействующий с клиентами и сервером на C.
- `index.html` — веб-интерфейс для игры.
- `public/` — директория для статических файлов.

## 🚀 Установка и запуск
### 1. Запуск C-сервера
```bash
gcc server.c -o server -lpthread
./server
```

### 2. Запуск Node.js сервера
Установите зависимости:
```bash
npm install express ws net
```
Запустите сервер:
```bash
node server.js
```

### 3. Запуск веб-клиента
Перейдите по адресу `http://localhost:3000'.

## ⚡ Использование
1. Зарегистрируйтесь и войдите в систему.
2. Создайте игру или присоединитесь к существующей.
3. Дождитесь начала и делайте ходы, нажимая на клетки поля.
4. Общайтесь с соперником через чат.

## 🔧 Технические детали
- **C-сервер**: Обрабатывает игровую логику, хранит состояние игр и управляет игроками.
- **Node.js сервер**: Является посредником между веб-клиентом и сервером на C.
- **WebSocket**: Используется для моментального обновления состояния игры.

🎮 Удачной игры!

