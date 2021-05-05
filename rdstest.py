import Si4703
import time

Si4703.initialize()

Si4703.goToChannel(927)

while True:
    data = Si4703.readRDS()
    print(data)
    time.sleep(1)
