# Airport

### 1. Ground Control (порт 8081)

```
GET /v1/land_permission?flightId=...
    Запрос разрешения на посадку (Board -> GroundControl)
    
POST /v1/flights/{flightId}/landed
    Информирование о посадке в RE-1 ноду карты аэропорта (Board -> GroundControl)
      
POST /v1/vehicles/init
    Информирование о всех созданных машинках FollowMe (FollowMe -> GroundControl)
     
GET /v1/map/followme/path?flightId=...
    Запрос маршрута для сопровождения севшего рейса до парковки (FollowMe -> GroundControl)
    
GET /v1/map/followme/permission?flightId=...
    Запрос разрешение на начало сопровождения севшего рейса до парковки (FollowMe -> GroundControl)

POST /v1/map/traffic/enter-edge
    Информирование о входе в ребро по маршруту от посадки до парковки самолета машинкой FollowMe (FollowMe -> GroundControl)
    
POST /v1/map/traffic/leave-edge
    Информирование о выезде из ребра по маршруту от посадки до парковки самолета машинкой FollowMe (FollowMe -> GroundControl)

POST /v1/vehicles/followme (status=arrivedParking)
    Информирование о статусе выполнения доставки самолета до парковки машинкой FollowMe в процессе выполнения (FollowMe -> GroundControl)

POST /v1/followme/mission/completed
    Информирование о выполнении доставки самолета до парковки машинкой FollowMe в итоге (FollowMe -> GroundControl)

GET /v1/visualizer/snapshot
    Получение актуального состояния системы аэропорта: карта с занятыми кем-то ребрами или узлами (Visualizer -> GroundControl)

GET /v1/visualizer/events?since=...
    Получение полного лога событий перемещения машинки FollowMe (в перспективе и всех других транспортов по аэропорту) с определенного момента времени (Visualizer -> GroundControl) 

GET /health
    Проверка, что сервис запустился (* -> GroundControl)
```

### 2. Information Panel (порт 8082)

```
POST /v1/flights/init
    Инициализация всех рейсов, которые запланированы к посадке с указанием времени ожидания
    P.S.: лучше заменить на чтение из БД

GET /v1/flights/{flightId}?ts=...
    Запрос, ожидаем ли мы посадку рейса в определенное время (GroundControl -> Information Table)

POST /v1/flights/status
    Изменение статуса рейса (ожидаем, сел, взлетел и так далее) (GroundControl -> Information Table)

* GET /v1/flights (не используется)
    Запрос всех запланированных рейсов (GroundControl -> Information Table)
   
```

### 3. FollowMe (порт 8083)

```
POST /v1/vehicles/init
    Запрос на создание машинок FollowMe на спецпарковке FS-1 (GroundControl -> FollowMe)

GET /v1/vehicles/hasEmpty
    Запрос, есть ли свободные машинки FollowMe: будет ли кому доставить самолет до парковки (GroundControl -> FollowMe)

POST /v1/vehicles/reserve
    Резервирование машинки FollowMe для обслуживания самолета (GroundControl -> FollowMe)

POST /v1/vehicles/release
    Отмена резервирование машинки FollowMe (GroundControl -> FollowMe)

* GET /v1/vehicles (не используется)
    Получение информации о всех машинках FollowMe (GroundControl -> FollowMe)

GET /health
    Проверка, что сервис запустился (* -> FollowMe)
```

Списки атрибутов для POST-запросов искать в коде.
В директории scripts можно вызывать скрипы для отдельных API-ручек.

Цель сборки *Board* компилит бинарь, который можно использовать для тестирования посадки самолета.

### Порядок запуска сервисов
1. Information Panel
2. Ground Control
3. FollowMe
