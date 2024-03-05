#include <cstdio>
#include <memory>
#include <mpi/mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <iostream>
#include <string>
#include <map>
#include <unordered_map>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

// am pus taguri numere aproape random doar ca sa nu se suprapuna
// accidental, nu stiu daca mi-a iesit

#define REQUEST_FILE -1
#define END -2
#define SEND_UPDATE -3
#define REQ_UPDATE -4
#define MSG_TAG 69000
#define PEER_REQUEST_TAG 69420

// data la care s-a lansat Kill'em all de la Metallica
#define KILL_EM_ALL 25071983
#define SEND_VIBE_CHECK 666
#define RECV_VIBE_CHECK 777

#define getName(var)  #var

// in loc sa fac o clasa pentru client si tracker am
// folosit namespace-uri diferite
namespace client {


    using file = struct file {
        // numele fisierului
        char filename[20];
        // numarul de segmente
        int nr_segments;
        // segmentele fisierului
        std::vector<char*> segments;
        // clientii care au un segment i
        std::vector<std::vector<int>> segment_peers;
        // pentru fisierele care nu sunt detinute la start
        // segmentele se descarca secvential
        // si last_segment marcheaza segmentul la care s-a
        // ajuns cu descarcarea
        int last_segment;

        // am folosit asta pentru debug
        void display(FILE* f) {
            fprintf(f, "%s\n%d\n", filename, nr_segments);
            for (int i = 0; i < segments.size(); i++) {
                fprintf(f, "%s\n", segments[i]);
            }
            fflush(f);
        }
    };

    // structura care retine toate datele pentru un anumit client
    using client_data = struct client_data {
        // numarul de fisiere pentru care clientul e seed initial
        // (le detine in momentul pornirii tracker-ului)
        int nr_owned_files;
        // fisierele descrise mai sus
        std::vector<std::shared_ptr<file>> owned_files;

        // numarul de fisiere pe care clientul doreste
        // sa le descarce
        int nr_downloading_files;
        // fisierele descrise mai sus
        std::vector<std::shared_ptr<file>> downloading_files;

        // functie folosita pentru debug
        void display(FILE* fl) {
            fprintf(fl, "%d\n", nr_owned_files);
            for (const auto& f : owned_files) {
                f->display(fl);
            }
            fprintf(fl, "%d\n", nr_downloading_files);
            for (int i = 0; i < downloading_files.size(); i++) {
                downloading_files[i]->display(fl);
            }
        }
    };

    client_data c_data;
    volatile bool uploading = true;
    FILE* debug;

    void send_updates(int rank) {
        int msg_total = c_data.downloading_files.size() + c_data.owned_files.size();
        int payload = SEND_UPDATE;
        // trimit situatia curenta a clientului la tracker
        // cu un payload specific pentru operatia pe care urmeaza sa o fac
        MPI_Send(&payload, 1, MPI_INT, 0, MSG_TAG, MPI_COMM_WORLD);
        MPI_Send(&msg_total, 1, MPI_INT, 0, SEND_VIBE_CHECK * rank, MPI_COMM_WORLD);

        // din moment ce descarc fisierele secvential, pot doar sa trimit indexul
        // ultimului segment descarcat pentru a marca progresul
        for (int i = 0; i < c_data.downloading_files.size(); i++) {
            MPI_Send(c_data.downloading_files[i]->filename, 20, MPI_CHAR, 0, SEND_VIBE_CHECK * rank, MPI_COMM_WORLD);
            MPI_Send(&c_data.downloading_files[i]->last_segment, 1, MPI_INT, 0,  SEND_VIBE_CHECK * rank + 1, MPI_COMM_WORLD);
        }

        // pentru fisierele detinute, indexul ultimului segment e chiar
        // numarul de segmente
        for (int i = 0; i < c_data.owned_files.size(); i++) {
            MPI_Send(c_data.owned_files[i]->filename, 20, MPI_CHAR, 0, SEND_VIBE_CHECK * rank, MPI_COMM_WORLD);
            MPI_Send(&c_data.owned_files[i]->nr_segments, 1, MPI_INT, 0, SEND_VIBE_CHECK * rank + 1, MPI_COMM_WORLD);
        }
    }

    void request_updates(int rank) {
        int payload = REQ_UPDATE;
        MPI_Send(&payload, 1, MPI_INT, 0, MSG_TAG, MPI_COMM_WORLD);
        payload = c_data.downloading_files.size();
        MPI_Send(&payload, 1, MPI_INT, 0, RECV_VIBE_CHECK * rank, MPI_COMM_WORLD);

        // pentru fiecare fisier
        for (int i = 0; i < c_data.downloading_files.size(); i++) {

            // trimit numele lui(pentru a-l putea identifica tracker-ul)
            MPI_Send(c_data.downloading_files[i]->filename, 20, MPI_CHAR, 0, RECV_VIBE_CHECK * rank, MPI_COMM_WORLD);
            
            // apoi pentru fiecare segment al lui
            for (int j = 0; j < c_data.downloading_files[i]->segments.size(); j++) {
                int num;

                // primesc numarul de clienti care au acel segment
                MPI_Recv(&num, 1, MPI_INT, 0, RECV_VIBE_CHECK * rank, MPI_COMM_WORLD, nullptr);
                
                // pentru fiecare client care are segmentul
                for (int k = 0; k < num; k++) {
                    int peer;
                    bool isalready = false;
                    
                    // primesc id-ul lui
                    MPI_Recv(&peer, 1, MPI_INT, 0, RECV_VIBE_CHECK * rank, MPI_COMM_WORLD, nullptr);
                    
                    // verific intr-un mod ineficient si urat daca exista deja in lista
                    // acelui segment
                    for (int l = 0; l < c_data.downloading_files[i]->segment_peers[j].size(); l++) {
                        if (c_data.downloading_files[i]->segment_peers[j][l] == peer) {
                            isalready = true;
                        }
                    }

                    // daca nu se afla in lista il adaug
                    if (isalready == false) {
                        c_data.downloading_files[i]->segment_peers[j].push_back(peer);
                    }
                }
            }
        }
    }

    // functie pentru scrierea in fisiere fizice a fisierelor transferate
    void write_to_file(int rank, std::shared_ptr<file> file) {
        char buffer[50];
        std::snprintf(buffer, 50, "client%d_%s", rank, file->filename);
        FILE* output = fopen(buffer, "w");
        for (int j = 0; j < file->nr_segments; j++) {
            fprintf(output, "%s", file->segments[j]);
            fflush(output);
            if (j != file->nr_segments - 1) {
                fprintf(output, "\n");
                fflush(output);
            }
        }
        fclose(output);
    }

    void *download_thread_func(void *arg)
    {
        int wait;
        int rank = *(int*) arg;

        // nu sunt sigur daca se intra pe cazul asta dar mi-am zis sa
        // il acopar just in case
        if (c_data.nr_downloading_files == 0) {
            wait = 0;
            MPI_Send(&wait, 1, MPI_INT, 0, MSG_TAG, MPI_COMM_WORLD);
        } else {

            // pentru fiecare fisier pe care il doreste un client
            for (int i = 0; i < c_data.nr_downloading_files; i++) {
                int payload = REQUEST_FILE;

                // trimit codul operatiei pe care vreau sa o fac
                MPI_Send(&payload, 1, MPI_INT, 0, MSG_TAG, MPI_COMM_WORLD);
                
                // trimit numele fisierului
                MPI_Send(c_data.downloading_files[i]->filename, 20, MPI_CHAR, 0, rank, MPI_COMM_WORLD);
                
                // primesc numarul de hash-uri al fisierului
                MPI_Recv(&c_data.downloading_files[i]->nr_segments, 1, MPI_INT, 0, rank, MPI_COMM_WORLD, nullptr);
                for (int j = 0; j < c_data.downloading_files[i]->nr_segments; j++) {
                    char* buffer = (char*)malloc(50);
                    
                    // primesc hash-urile efectiv
                    MPI_Recv(buffer, 33, MPI_CHAR, 0, j, MPI_COMM_WORLD, nullptr);
                    c_data.downloading_files[i]->segments.push_back(buffer);
                    short num;

                    // primesc lista de clienti care au hash-ul respectiv 
                    MPI_Recv(&num, 1, MPI_SHORT, 0, j, MPI_COMM_WORLD, nullptr);
                    std::vector<int> v;
                    for (int k = 0; k < num; k++) {
                        int peer;
                        MPI_Recv(&peer, 1, MPI_INT, 0, j, MPI_COMM_WORLD, nullptr);
                        v.push_back(peer);
                    }
                    c_data.downloading_files[i]->segment_peers.push_back(v);
                }
            }
            // seg_counter e folosit pentru a implementa partea aceea din cerinta
            // care zice ca la fiecare 10 segmente descarcate sa dam update la tracker
            // si sa cerem update
            int seg_counter = 0;
            while (c_data.nr_downloading_files != 0) {
                for (int i = 0; i < c_data.downloading_files.size(); i++) {
                    int index = c_data.downloading_files[i]->last_segment;

                    // pentru a distribui eficient load-ul la clienti
                    // aleg din lista de clienti care au segmentul cautat
                    // un client random
                    int peer_index = rand() % c_data.downloading_files[i]->segment_peers[index].size();
                    int peer = c_data.downloading_files[i]->segment_peers[index][peer_index];
                    char *buf = c_data.downloading_files[i]->segments[index];
                    int resp;

                    // trimit la clientul selectat hash-ul
                    MPI_Send(buf, 33, MPI_CHAR, peer, PEER_REQUEST_TAG, MPI_COMM_WORLD);
                    int flag = 0;
                    MPI_Status status;
                    MPI_Request r;

                    // astept raspunsul de la client
                    int ret = MPI_Recv(&resp, 1, MPI_INT, peer, PEER_REQUEST_TAG + peer, MPI_COMM_WORLD, &status);
                    
                    // marchez pentru fisierul respectiv ca am mai descarcat un segment
                    c_data.downloading_files[i]->last_segment++;

                    // verific cazul in care am terminat de descarcat un fisier
                    if (c_data.downloading_files[i]->last_segment >= c_data.downloading_files[i]->segments.size()) {
                        write_to_file(rank, c_data.downloading_files[i]);
                        c_data.owned_files.push_back(c_data.downloading_files[i]);
                        c_data.downloading_files.erase(c_data.downloading_files.begin() + i);
                        c_data.nr_downloading_files--;
                        i--;
                    }
                    seg_counter++;
                    if (seg_counter == 10) {
                        // functii pentru trimis si primit update-uri legate de situatia din swarm
                        send_updates(rank);
                        request_updates(rank);
                        seg_counter = 0;
                    }
                }
            }
        }

        // se iese din while cand se termina de descarcat toate fisierele
        // si trimit aceasta informatie la tracker
        int payload = END;
        MPI_Send(&payload, 1, MPI_INT, 0, MSG_TAG, MPI_COMM_WORLD);
        return NULL;
    }

    // functia pentru thread-ul de upload
    void *upload_thread_func(void *arg)
    {
        int rank = *(int*) arg;
        MPI_Request r;
        char buffer[50];
        while (uploading) {
            // am folosit Irecv pentru a nu bloca thread-ul acesta
            // cat timp astept mesaje de la alti clienti
            MPI_Irecv(&buffer, 33, MPI_CHAR, MPI_ANY_SOURCE, PEER_REQUEST_TAG, MPI_COMM_WORLD, &r);
            int flag = 0;
            MPI_Status status;
            MPI_Test(&r, &flag, &status);

            // apelez MPI_Test cat timp request-ul de recv nu e satisfacut
            while (flag == 0 && uploading) {
                MPI_Test(&r, &flag, &status);
            }

            // trimit ack-ul la clientul care a cerut un segment
            int* yes = (int*)malloc(sizeof(int));
            *yes = 1;
            MPI_Send(yes, 1, MPI_INT, status.MPI_SOURCE, PEER_REQUEST_TAG + rank, MPI_COMM_WORLD);
        }
        return NULL;
    }

    // functie de citirea a fisierelor initiale
    void read_input_file_peer(int rank) {
        char buffer[50];
        std::snprintf(buffer, 50, "in%d.txt", rank);
        FILE* input_file = fopen(buffer, "r");

        // citesc fisierul conform formatului si extrag datele din el
        fgets(buffer, 50, input_file);
        sscanf(buffer, "%d", &c_data.nr_owned_files);
        for (int i = 0; i < c_data.nr_owned_files; i++) {
            fgets(buffer, 50, input_file);
            std::shared_ptr<file> f = std::make_shared<file>();
            sscanf(buffer, "%s %d", f->filename, &f->nr_segments);
            f->last_segment = f->nr_segments;
            for (int j = 0; j < f->nr_segments; j++) {
                char* hash = (char*)malloc(50 * sizeof(char));
                fgets(hash, 50, input_file);
                // scot \n de la finalul hash-urilors
                if (hash[strlen(hash) - 1] == '\n') {
                    hash[strlen(hash) - 1] = 0;
                }
                f->segments.push_back(hash);
            }
            c_data.owned_files.push_back(f);
        }
        fgets(buffer, 50, input_file);
        sscanf(buffer, "%d", &c_data.nr_downloading_files);
        for (int i = 0; i < c_data.nr_downloading_files; i++) {
            std::shared_ptr<file> f = std::make_shared<file>();
            f->last_segment = 0;
            char* hash = (char*)malloc(50);
            fgets(hash, 50, input_file);
            // la fel ca mai sus
            if (hash[strlen(hash) - 1] == '\n') {
                hash[strlen(hash) - 1] = 0;
            }
            memcpy(f->filename, hash, 20);
            c_data.downloading_files.push_back(f);
        }
    }

    void peer(int numtasks, int rank) {
        pthread_t download_thread;
        pthread_t upload_thread;
        void *status;
        int r;

        char buffer[50];

        read_input_file_peer(rank);

        // trimit configuratia initiala la tracker
        MPI_Send(&c_data.nr_owned_files, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);

        // trimit toate fisierele detinute cu toate segmentele lor
        for (const auto& owned_file: c_data.owned_files) {
            MPI_Send(owned_file->filename, 20, MPI_CHAR, 0, 1, MPI_COMM_WORLD);
            MPI_Send(&owned_file->nr_segments, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);
            for (int i = 0; i < owned_file->nr_segments; i++) {
                MPI_Send(owned_file->segments[i], 33, MPI_CHAR, 0, i, MPI_COMM_WORLD);
            }
        }
        int should_continue;

        MPI_Status feedback;

        // primesc unda verde de la tracker ca pot incepe descarcarea
        MPI_Recv(&should_continue, 1, MPI_INT, 0, MPI_ANY_TAG, MPI_COMM_WORLD, &feedback);


        r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
        if (r) {
            printf("Eroare la crearea thread-ului de download\n");
            exit(-1);
        }

        r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
        if (r) {
            printf("Eroare la crearea thread-ului de upload\n");
            exit(-1);
        }

        r = pthread_join(download_thread, &status);
        if (r) {
            printf("Eroare la asteptarea thread-ului de download\n");
            exit(-1);
        }

        int sth;
        // cu thread-ul de download deja oprit, astept mesaj de la tracker
        // pentru a putea opri si thread-ul de upload
        MPI_Recv(&sth, 1, MPI_INT, 0, KILL_EM_ALL, MPI_COMM_WORLD, nullptr);
        uploading = false;

        r = pthread_join(upload_thread, &status);
        if (r) {
            printf("Eroare la asteptarea thread-ului de upload\n");
            exit(-1);
        }
    }
}

namespace tracker {

    // nu aveam idee de alt nume pentru structura si parea funny
    // initial voiam sa folosesc connected_to pentru load balancing
    // dar apoi am renuntat la idee
    using pirate = struct pirate {
        int rank;
        int connected_to;
    };

    using pirate_list = struct pirate_list {
        // contine hash-ul segmentului si ce clienti detin segmentul respectiv
        std::string hash;
        std::vector<std::shared_ptr<pirate>> vect;
    };

    using file_swarm = struct file_swarm {
        // numele fisierului
        std::string name;

        // numarul de segmente al fisierului
        int nr_segments;

        // lista segmentelor
        std::vector<pirate_list> segment_list;
    };

    std::vector<std::shared_ptr<file_swarm>> files;
    FILE* master;

    void initialize(int numtasks, int rank) {
        for (int i = 1; i < numtasks; i++) {
            int number_of_files;
            MPI_Status status;

            // primesc numarul de fisiere pe care il detine un client
            MPI_Recv(&number_of_files, 1, MPI_INT, MPI_ANY_SOURCE, 1, MPI_COMM_WORLD, &status);
            if (number_of_files < 0) {
                continue;
            }


            auto current_pirate = std::make_shared<pirate>();
            current_pirate->connected_to = 0;
            current_pirate->rank = status.MPI_SOURCE;

            // pentru fiecare fisier
            for (int i = 0; i < number_of_files; i++) {
                char buffer[50];
                int number_of_segments;

                // primesc numele fisierului
                MPI_Recv(buffer, 20, MPI_CHAR, status.MPI_SOURCE, 1, MPI_COMM_WORLD, nullptr);

                auto file_name = std::string(buffer);


                // verific daca mai tin evidenta pentru acel fisier
                std::shared_ptr<file_swarm> current_file_swarm;
                bool already_in = false;
                for (const auto& fl: files) {
                    if (file_name == fl->name) {
                        already_in = true;
                        current_file_swarm = fl;
                    }
                }
                if (!already_in) {
                    current_file_swarm = std::make_shared<file_swarm>();
                    current_file_swarm->name = file_name;
                    files.emplace_back(current_file_swarm);
                }


                // primesc numarul de segmente al fisierului
                MPI_Recv(&number_of_segments, 1, MPI_INT, status.MPI_SOURCE, 1, MPI_COMM_WORLD, nullptr);

                current_file_swarm->nr_segments = number_of_segments;

                // si primesc hash-urile segmentului
                for (int i = 0; i < number_of_segments; i++) {
                    MPI_Recv(buffer, 33, MPI_CHAR, status.MPI_SOURCE, i, MPI_COMM_WORLD, nullptr);
                    already_in = false;
                    int index = 0;
                    for (const auto& hs:current_file_swarm->segment_list) {
                        if (hs.hash == std::string(buffer)) {
                            already_in = true;
                            break;
                        }
                        index++;
                    }

                    // adaug hash-ul in lista de hash-uri sau adaug clientul in lista de owneri a segmentului
                    if (!already_in) {
                        pirate_list p;
                        p.hash = std::string(buffer);
                        p.vect.push_back(current_pirate);
                        current_file_swarm->segment_list.emplace_back(p);
                    } else {
                        current_file_swarm->segment_list[index].vect.emplace_back(current_pirate);
                    }
                }
            }
            int go = 1;
            // trimit mesaj clientului ca poate incepe descarcarea
            MPI_Send(&go, 1, MPI_INT, status.MPI_SOURCE, 0, MPI_COMM_WORLD);
        }
    }

    void tracker(int numtasks, int rank) {
        initialize(numtasks, rank);

        int end_counter = 0;
        int counter = 0;
        while (true) {
            int resp;
            MPI_Status status;
            // in resp se retine ce operatie vrea sa faca un client
            MPI_Recv(&resp, 1, MPI_INT, MPI_ANY_SOURCE, MSG_TAG, MPI_COMM_WORLD, &status);
            
            // daca un client a ajuns la final, se marcheaza acest lucru
            if (resp == END) {
                end_counter++;
                // daca toti clientii au ajuns la final, se iese din bucla de rulare
                if (end_counter >= numtasks - 1) {
                    break;
                }
                continue;
            }

            // daca un client cere un anumit fisier
            if (resp == REQUEST_FILE) {
                char buffer[20];

                // primesc numele lui
                MPI_Recv(buffer, 20, MPI_CHAR, status.MPI_SOURCE, status.MPI_SOURCE, MPI_COMM_WORLD, &status);
                std::shared_ptr<file_swarm> current_file = nullptr;
                
                // il caut in lista intr-un mod ineficient
                for (const auto& fl : files) {
                    if (fl->name == std::string(buffer)) {
                        current_file = fl;
                        break;
                    }
                }
                if (current_file == nullptr) {
                    continue;
                }
                counter = 0;

                // trimit numarul de segmente
                MPI_Send(&current_file->nr_segments, 1, MPI_INT, status.MPI_SOURCE, status.MPI_SOURCE, MPI_COMM_WORLD);
                for (int k = 0; k < current_file->segment_list.size();k++) {
                    
                    // trimit hash-ul fiecarui segment
                    MPI_Send(current_file->segment_list[k].hash.c_str(), 33, MPI_CHAR, status.MPI_SOURCE, k, MPI_COMM_WORLD);
                    short num = current_file->segment_list[k].vect.size();

                    // trimit numarul de clienti care au segmentul
                    MPI_Send(&num, 1, MPI_SHORT, status.MPI_SOURCE, k, MPI_COMM_WORLD);
                    for (int l = 0; l < num; l++) {

                        // si care clienti sunt aceia
                        MPI_Send(&current_file->segment_list[k].vect[l]->rank, 1, MPI_INT, status.MPI_SOURCE, k, MPI_COMM_WORLD);
                    }
                }
                continue;
            }

            // daca clientul vrea sa isi transmita progresul
            if (resp == SEND_UPDATE) {
                int msg_total;

                // trimite numarul de fisiere la care e implicat
                MPI_Recv(&msg_total, 1, MPI_INT, status.MPI_SOURCE, SEND_VIBE_CHECK * status.MPI_SOURCE, MPI_COMM_WORLD, nullptr);
                for (int i = 0; i < msg_total; i++) {
                    int limit_for_file = 0;
                    char buffer[30];

                    // primesc numele fisierului si ultimul segment pe care il are clientul din fisier
                    MPI_Recv(buffer, 20, MPI_CHAR, status.MPI_SOURCE, SEND_VIBE_CHECK * status.MPI_SOURCE, MPI_COMM_WORLD, nullptr);
                    MPI_Recv(&limit_for_file, 1, MPI_INT, status.MPI_SOURCE, SEND_VIBE_CHECK * status.MPI_SOURCE + 1, MPI_COMM_WORLD, nullptr);
                    std::shared_ptr<file_swarm> req_file = nullptr;
                    for (int j = 0; j < files.size(); j++) {
                        if (files[j]->name == std::string(buffer)) {
                            req_file = files[j];
                            break;
                        }
                    }
                    if (req_file == nullptr) {
                        continue;
                    }

                    // actualizez lista de owneri a segmenelor descarcate ulterior
                    for (int j = 0; j < limit_for_file; j++) {
                        bool isalready = false;
                        for (int k = 0; k < req_file->segment_list[j].vect.size(); k++) {
                            if (req_file->segment_list[j].vect[k]->rank == status.MPI_SOURCE) {
                                isalready = true;
                            }
                        }
                        if (!isalready) {
                            auto ptr = std::make_shared<pirate>();
                            ptr->rank = status.MPI_SOURCE;
                            ptr->connected_to = 0;
                            req_file->segment_list[j].vect.push_back(ptr);
                        }
                    }
                }
                continue;
            }

            // daca un client vrea un update la situatia curenta
            if (resp == REQ_UPDATE) {
                int num;
                // primesc numarul de fisiere pe care le vrea
                MPI_Recv(&num, 1, MPI_INT, status.MPI_SOURCE, RECV_VIBE_CHECK * status.MPI_SOURCE, MPI_COMM_WORLD, nullptr);
                for (int i = 0; i < num; i++) {
                    char buffer[20];
                    // apoi numele fiecarui fisier
                    MPI_Recv(buffer, 20, MPI_CHAR, status.MPI_SOURCE, RECV_VIBE_CHECK * status.MPI_SOURCE, MPI_COMM_WORLD, nullptr);
                    std::shared_ptr<file_swarm> current_file = nullptr;
                    for (int j = 0; j < files.size(); j++) {
                        if (files[j]->name == std::string(buffer)) {
                            current_file = files[j];
                            break;
                        }
                    }
                    // pentru fiecare fisier, iau fiecare segment
                    for (int j = 0; j < current_file->segment_list.size(); j++) {
                        int payload = current_file->segment_list[j].vect.size();
                        // trimit numarul de clienti care are segmentul respectiv
                        MPI_Send(&payload, 1, MPI_INT, status.MPI_SOURCE, RECV_VIBE_CHECK * status.MPI_SOURCE, MPI_COMM_WORLD);
                        for (int k = 0; k < current_file->segment_list[j].vect.size(); k++) {
                            payload = current_file->segment_list[j].vect[k]->rank;
                            // si clientii aceia efectiv
                            MPI_Send(&payload, 1, MPI_INT, status.MPI_SOURCE, RECV_VIBE_CHECK * status.MPI_SOURCE, MPI_COMM_WORLD);
                        }
                    }
                }
                continue;
            }
        }
        // daca s-a iesit din bucla principala inseamna ca toti clientii
        // au terminat de descarcat, deci semmnalez ca isi pot inchide
        // si thread-urile de upload
        for (int i = 1; i < numtasks; i++) {
            int sth = 1;
            MPI_Send(&sth, 1, MPI_INT, i, KILL_EM_ALL, MPI_COMM_WORLD);
        }
    }
}

int main (int argc, char *argv[]) {
    int numtasks, rank;

    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker::tracker(numtasks, rank);
    } else {
        client::peer(numtasks, rank);
    }

    MPI_Finalize();
}
