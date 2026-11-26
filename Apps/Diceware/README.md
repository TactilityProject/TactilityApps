# Diceware

This application is based on the [Diceware](https://en.wikipedia.org/wiki/Diceware) passphrase generator.

In a regular Diceware scenario, you roll 6 dice and match it with the corresponding word from the word list.
In this application, it is simplified by getting a 32 bit random integer to get a random word from the word list.

The application asks to enable Wi-Fi because of the hardware [random generator on the ESP32](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/random.html).
You can enable the Wi-Fi radio to improve the randomness. You don't have to be connected to any network.
If you automatically connect to a Wi-Fi network, go to the network settings and manually disconnect from it.

## Usage

A passphrase is shown when the app opens. Press **Roll** for a new one, and use **−** and **+** to choose 4 to 12 words.
The meter and the line below it show the strength of the phrase: the number of bits, and roughly how long it would take to find at a trillion guesses per second.
