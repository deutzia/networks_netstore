AUTHOR'S NOTES:
Zaproponowanie przeze mnie rozwiązanie nie jest w pełni poprawne, ponieważ
rozwiązanie musi opierać się o protokół przedstawiony w treści zadania, a więc
używać UDP, czyli nie ma żadnej pewności, że da się dowiedzieć o wszystkich
pozostałych serwerach w grupie.
Myślałam o tym, żeby używać jakiejś weryfikacji, czy serwer dostał odpowiedź
od innego serwera, ale ten tok rozumowania badzo szybko prowadzi do
reimplementowania TCP od nowa, czego zakładam, że nie powinnam robić.
Do poprawności więc zakładam, że pakiety raczej dochodzą, że da się
porozmawiać z innymi serwerami.

WŁAŚCIWE ROZWIĄZANIE:
Mamy więc do obsłużenia następujące sytuacje zmieniające stan grupy;
    1) dołącza nowy serwer
    2) plik zostaje usunięty
    3) plik zostaje dodany
    4) odłącza się serwer
Proponuję, aby każdy serwer trzymał informację nie tylko o swoich plikach, ale
także o plikach innych serwerów i aktualizował ją na bieżąco. Stąd mamy:

1) Serwer w momencie dołączenia musi dowiedzieć się, jakie pliki mają inne
serwery. W tym celu wykonuje wysyła LIST zgodnie z formatem opisanym w treści
i dostaje odpowiedzi MY_LIST. Pojawia się pytanie, co, jeżeli ten nowy serwer
ma zindeksowane pliki o nazwach, które już istnieją na innych serwerach. Moja
pierwsza propozycja to - nie udostępnia ich, traktuje je, jakby ich nie było
(co też znaczy, że nie usuwa ich przy DEL). To może powodować jednak
nadpisanie tych plików, jeśli odłączy się serwer, który je miał. Druga
propozycja, to przełożyć pliki do jakiegoś katalogu, ale wydaje mi się, że to
może być dla użytkownika bardzo konfundujące. Trzecia, na którą się
zdecydowałam, to kazać użytkownikowi to naprawić, a więc wypisać odpowiedni
komuniat o błędzie i zakończyć działanie.
Dodatkowo także serwer musi także poinformować inne serwery o tym, jakie ma
pliki. W tym celu wysyła im simpl_cmd o polu CMD = "JOINING" i polu data
składającym się z nazw plików oddzielonych znakami nowej linii (być może
wysyła wiele razy, tak, jak przy wysyłaniu MY_LIST).

2) DEL wysyłane jest na adres multicast, a więc serwer usuwa plik nie swojego
dysku, ale także z listy plików posiadanych przez inne serwery.

3) Przed wysłaniem zgody na dodanie pliku serwer sprawdza, czy jakiś inny
serwer nie ma pliku o podanej nazwie i jeśli tak jest, to odpowiada NO_WAY
zgodnie z treścią zadania.
Co, jeśli żaden serwer nie ma pliku o zadanej nazwie? Pierwsza propozycja, to
od razu zaakceptować i poinformować inne serwery, że od teraz ma się taki
plik. W ten sposób może jednak powstać dwa razy plik o tej samej nazwie jeśli
dwaj różni klienci wyślą do różnych serwerów ADD w tym samym czasie i oba
serwery się zgodzą.
Drugą propozycją jest więc użycie jakiegoś mechanizmu synchronizacji węzłów (w
stylu tych przedstawionych na programowaniu współbieżnym). Niestety, ze
względu na specyfikę UDP nie ma możliwości zastosowania żadnego algortymu z
krążącym żetonem (np serwer czeka na żeton przez X czasu, jeśli nie dostanie,
to odpowiada NO_WAY), ponieważ pakiet z żetonem może się zgubić, co
sprawiłoby, że nie dałoby się dodawać w ogóle plików do grupy. Z tego powodu
nie zdecydowałam się na to rozwiązanie.
Trzecią popozycją jest więc, żeby serwer na wszelki wypadek wysyłał do innych
DEL. Wówczas we wskazanej przeze mnie sytuacji w pierwszej propozycji oba
serwery wyślą DEL do pozostałych, a więc ostatecznie żaden z nich nie będzie
miał tego pliku.

4) W momencie odłączania się serwera od grupy musi on poinformować pozostałe
serwery o tym, że się odłącza. W tym cely wysyła im simpl_cmd o polu CMD =
"LEAVING" i pustym polu data.

