#!/bin/bash

# Mergem in directorul proiectului
cd ~/Desktop/Proiect/PCD

# Creare fisier mare de test
echo "Creare fisier mare de test..."
> large_test.csv
for i in {1..10000}; do
    echo "44.${i},26.${i}" >> large_test.csv
done

echo "Fisier creat: $(wc -l large_test.csv) linii"

# Asigura-te ca userul sad exista in passwords.txt
if ! grep -q "^sad " passwords.txt; then
    echo "Adaugam utilizatorul sad in passwords.txt..."
    echo "sad sad123" >> passwords.txt
fi

# Porneste serverul daca nu ruleaza
if ! pgrep -x "serverds" > /dev/null; then
    echo "Pornim serverul..."
    ./serverds &
    sleep 2
fi

# Porneste 3 clienti in paralel
echo "Pornire clienti in paralel..."

# Client 1 - fisier mare (C client)
(
    echo "1"
    echo "sad"
    echo "sad123"
    echo "upload large_test.csv"
    echo "exit"
) | ./clients/inetclient > client1.log 2>&1 &

# Client 2 - Python client
(
    cd clients/ordinary_python
    python3 -c "
from ordinary_client import GeoClient
c = GeoClient()
c.connect()
if c.do_login():
    c.upload_file('../../large_test.csv')
    c.close()
" 
) > client2.log 2>&1 &

# Client 3 - fisier normal (C client)
(
    echo "1"
    echo "sad"
    echo "sad123"
    echo "upload test.csv"
    echo "exit"
) | ./clients/inetclient > client3.log 2>&1 &

wait
echo "Toti clientii au terminat"

# Afiseaza rezultate
echo ""
echo "=== REZULTATE CLIENT 1 ==="
tail -20 client1.log
echo ""
echo "=== REZULTATE CLIENT 2 ==="
tail -20 client2.log
echo ""
echo "=== REZULTATE CLIENT 3 ==="
tail -20 client3.log

# Curatenie
rm -f large_test.csv client1.log client2.log client3.log
