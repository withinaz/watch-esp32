idf_component_register(
    SRCS "main.cpp" "app_settings.cpp" "app_assistant.cpp" "app_music.cpp"
         "xiaozhi_client.cpp"
         "icons/app_icon_settings.c" "icons/app_icon_assistant.c"
         "icons/app_icon_music.c"
    INCLUDE_DIRS "."
)

# NE PAS ajouter -O2 ici. Le projet est compile en -Og, ce qui coute de la
# marge au moteur audio, mais un -O2 sur ce composant a provoque un gel du
# systeme une seconde apres l'ouverture de l'app Musique (plus aucun journal,
# ni plantage ni watchdog). La marge s'obtient en reduisant le travail du
# moteur, pas par l'optimisation.
