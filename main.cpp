#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <limits.h>

#include <string>
#include <set>
#include <map>
#include <vector>
#include <algorithm>

extern "C" {
  #include "fold.h"
  #include "findpath.h"
  #include "RNAlocmin_cmdline.h"
  #include "utils.h"
  #include "read_epars.h"
  #include "fold_vars.h"
}

#include "hash_util.h"
#include "globals.h"
#include "move_set.h"
#include "flood.h"

#include "barrier_tree.h"

using namespace std;

inline bool isSeq(char *p)
{
  // check first two chars - should be enough
  char seq[]="ACGTUactgu";
  int ok = 0;
  for (unsigned int i=0; i<strlen(seq); i++) {
    if (p[0]==seq[i]) {
      ok++;
    }
    if (p[1]==seq[i]) {
      ok++;
    }
    if (ok==2) break;
  }

  return (ok==2);
}

inline bool isStruct(char *p)
{
  // check first two chars - should be enough
  if ((p[0]=='.' || p[0]=='(' || p[0]==')') && (p[1]=='.' || p[1]=='(' || p[1]==')')) return true;
  else return false;
}

inline bool isEnergy(char *p, float &energy)
{
  if (sscanf(p, "%f", &energy) == 1) return true;
  else return false;
}

inline int en_fltoi(float en)
{
  if (en < 0.0) return (int)(en*100 - 0.5);
  else return (int)(en*100 + 0.5);
}

struct barr_info { // info taken from barriers
  int father;
  int e_diff;
  int bsize;
  int fbsize;
  // unused, unmodified
  float fen;
  int grad;
  float feng;

  barr_info &operator+(barr_info &second) {
    bsize += second.bsize;
    if (second.father!=father) father = -1;
    fbsize += second.fbsize;
    grad += second.grad;
    return *this;
  }
};

// functions that are down in file ;-)
char *read_seq(char *seq_arg, char **name_out);
int move(unordered_map<hash_entry, gw_struct, hash_fncts> &structs, map<hash_entry, int, compare_map> &output, set<hash_entry, compare_map> &output_shallow);
char *read_previous(char *previous, map<hash_entry, int, compare_map> &output);
char *read_barr(char *previous, map<hash_entry, barr_info, compare_map> &output);

int main(int argc, char **argv)
{
  clock_t clck1 = clock();

  // parse arguments
  gengetopt_args_info args_info;
  if (cmdline_parser(argc, argv, &args_info) != 0) {
    fprintf(stderr, "ERROR: argument parsing problem.\n");
    exit(EXIT_FAILURE);
  }

  //check for bad arguments
  if (Opt.Init(args_info) !=0 ) {
    fprintf(stderr, "ERROR: one or more bad arguments, exiting...\n");
    exit(EXIT_FAILURE);
  }

  // read parameter file
  if (args_info.paramFile_given) {
    read_parameter_file(args_info.paramFile_arg);
  }

  // dangle setup
  if (args_info.dangles_given) {
    dangles = args_info.dangles_arg;
  }

  // keep track of structures & statistics
  map<hash_entry, int, compare_map> output; // structures plus energies to output (+ how many hits has this minima)
  map<hash_entry, barr_info, compare_map> output_barr; // structures plus energies to output (+ barr_info)
  set<hash_entry, compare_map> output_shallow; // shallow structures (if minh specified)
  char *seq = NULL;
  char *name = NULL;

  // read previous LM or/and sequence
  if (args_info.fix_barriers_given) {
    seq = read_barr(args_info.fix_barriers_arg, output_barr);
  } else {
    if (args_info.previous_given) {
      seq = read_previous(args_info.previous_arg, output);
    } else {
      seq = read_seq(args_info.seq_arg, &name);
    }
  }

  if (args_info.verbose_lvl_arg>1) fprintf(stderr, "%s\n", seq);

  seq_len = strlen(seq);

  // time?
  if (args_info.verbose_lvl_arg>0) {
    fprintf(stderr, "Time to initialize: %.2f secs.\n", (clock() - clck1)/(double)CLOCKS_PER_SEC);
    clck1 = clock();
  }

  // ########################## begin main loop - reads structures from RNAsubopt and process them
  int count = max(output.size(), output_barr.size());  //num of local minima
  Enc.Init(seq);

  int not_canonical = 0;

  if (!args_info.fix_barriers_given) {
    // hash
    unordered_map<hash_entry, gw_struct, hash_fncts> structs (HASHSIZE);
    while (!args_info.find_num_given || count != args_info.find_num_arg) {
      int res = move(structs, output, output_shallow);
      if (res==0)   continue; // same structure has been processed already
      if (res==-1)  break;
      if (res==-2)  not_canonical++;
      if (res==1)   count=output.size();
    }


    // ########################## end main loop - reads structures from RNAsubopt and process them

    // free hash
    //int num_of_structures = hash_size();
    if (args_info.verbose_lvl_arg>0) print_stats(structs);
    add_stats(structs, output);
    free_hash(structs);

    // time?
    if (args_info.verbose_lvl_arg>0) {
      fprintf(stderr, "Main loop (deepest descent from RNAsubopt): %.2f secs.\n", (clock() - clck1)/(double)CLOCKS_PER_SEC);
      clck1 = clock();
    }

    // vectors for rates computation
    int num = ((count > args_info.min_num_arg && args_info.min_num_arg!=0) ? args_info.min_num_arg : count);
    vector<string> output_str;
    output_str.resize(num);
    vector<hash_entry> output_he;
    hash_entry h;
    h.structure = NULL;
    output_he.resize(num, h);
    vector<int> output_en;
    output_en.resize(num);
    vector<int> output_num;
    output_num.resize(num);

    // threshold for flooding
    int threshold;

    int i=0;
    for (map<hash_entry, int, compare_map>::iterator it=output.begin(); it!=output.end(); it++) {
      // if not enough minima
      if (i<num) {
        // first check if the output is not shallow
        if (Opt.minh>0) {
          int saddle;
          hash_entry *escape = flood(it->first, saddle, Opt.minh);

          // shallow
          if (escape) {
            if (args_info.verbose_lvl_arg>0) {
              fprintf(stderr, "shallow: %s %6.2f (saddle: %s %6.2f)\n", pt_to_str(it->first.structure).c_str(), it->first.energy/100.0, pt_to_str(escape->structure).c_str(), escape->energy/100.0);
            }
            free_entry(escape);
            free(it->first.structure);
            continue;
          }
        }
        // then add it to outputs.
        if (args_info.noSort_flag) {
          if (it->first.num<num) {
            output_str[it->first.num]=pt_to_str(it->first.structure);
            output_num[it->first.num]=it->second;
            output_he[it->first.num]=it->first;
            output_en[it->first.num]=it->first.energy;
            //printf("%d ", it->first.num);
            i++;
          }
        } else {
          if (args_info.verbose_lvl_arg > 2) fprintf(stderr, "%4d %s %6.2f\n", i, pt_to_str(it->first.structure).c_str(), it->first.energy/100.0);
          output_str[i]=pt_to_str(it->first.structure);
          output_num[i]=it->second;
          output_he[i]=it->first;
          output_en[i]=it->first.energy;
          i++;
        }
      } else { // we have enough minima
        free(it->first.structure);
      }
    }
    output.clear();

    // erase possible NULL elements...
    if (args_info.noSort_flag) {
      for (int j=output_he.size()-1; j>=0; j--) {
        if (output_he[j].structure==NULL) {
          output_he.erase(output_he.begin()+j);
          output_str.erase(output_str.begin()+j);
          output_en.erase(output_en.begin()+j);
          output_num.erase(output_num.begin()+j);
        }
      }
    } else {
      output_he.resize(i);
      output_str.resize(i);
      output_en.resize(i);
      output_num.resize(i);
    }

    // threshold for flooding
    vector<int> tmp = output_num;
    sort(tmp.begin(), tmp.end());
    int thr = num*args_info.floodPortion_arg;
    thr--;
    threshold = (thr<0 ? 0 : tmp[thr]);

      // time?
    if (args_info.verbose_lvl_arg>0) {
      fprintf(stderr, "Discarding shallow minima: %.2f secs.\n", (clock() - clck1)/(double)CLOCKS_PER_SEC);
      clck1 = clock();
    }

    // array of energy barriers
    float *energy_barr = NULL;
    bool *findpath_barr = NULL;

    // find saddles - fill energy barriers
    if (args_info.rates_flag || args_info.bartree_flag) {
      nodeT nodes[num];
      energy_barr = (float*) malloc(num*num*sizeof(float));
      for (int i=0; i<num*num; i++) energy_barr[i]=1e10;
      findpath_barr = (bool*) malloc(num*num*sizeof(bool));
      for (int i=0; i<num*num; i++) findpath_barr[i]=false;

      // fill nodes
      for (int i=0; i<num; i++) {
        nodes[i].father = -1;
        nodes[i].height = output_en[i]/100.0;
        nodes[i].label = NULL;
        nodes[i].color = 0.0;
        nodes[i].saddle_height = 1e10;
      }

      int flooded = 0;
      // init union-findset
      init_union(num);
      // first try to flood the highest bins
      for (int i=num-1; i>=0; i--) {
        // flood only if low number of walks ended there
        if (output_num[i]<=threshold) {
          //copy_arr(Enc.pt, output_he[i].structure);
          if (args_info.verbose_lvl_arg>2) fprintf(stderr,   "flooding  (%3d): %s %.2f\n", i, output_str[i].c_str(), output_he[i].energy/100.0);

          int saddle;
          hash_entry *he = flood(output_he[i], saddle);

          // print info
          if (args_info.verbose_lvl_arg>1) {
            if (he) {
              fprintf(stderr, "below     (%3d): %s %.2f\n"
                              "en: %7.2f  is: %s %.2f\n", i,
                      output_str[i].c_str(), output_he[i].energy/100.0, saddle/100.0,
                      pt_to_str(he->structure).c_str(), he->energy/100.0);
            } else {
              fprintf(stderr, "unsucesful(%3d): %s %.2f\n", i,
                      output_str[i].c_str(), output_he[i].energy/100.0);
            }
          }
          // if flood succesfull - walk down to find father minima
          if (he) {
            // walk down
            while (move_set(*he)!=0) {
              Deg.Clear();
            };
            Deg.Clear();

            // now check if we have the minimum already (hopefuly yes ;-) )
            vector<hash_entry>::iterator it;
            it = lower_bound(output_he.begin(), output_he.end(), *he, compare_vect);

            if (args_info.verbose_lvl_arg>1) fprintf(stderr, "minimum: %s %.2f\n", pt_to_str(he->structure).c_str(), he->energy/100.0);
            // we dont need it again
            free_entry(he);

            if (it!=output_he.end()) {
              int pos = (int)(it-output_he.begin());
              if (args_info.verbose_lvl_arg>1) fprintf(stderr, "found father at pos: %d\n", pos);

              flooded++;
              energy_barr[i*num+pos] = energy_barr[pos*num+i] = saddle/100.0;

              // union set
              //fprintf(stderr, "join: %d %d\n", min(i, pos), max(i, pos));
              union_set(min(i, pos), max(i, pos));
            }
          }
        }
      }

      // time?
      if (args_info.verbose_lvl_arg>0) {
        fprintf(stderr, "Flood(%d(%d)/%d): %.2f secs.\n", flooded, (int)(num*args_info.floodPortion_arg), num, (clock() - clck1)/(double)CLOCKS_PER_SEC);
        clck1 = clock();
      }

      // for others, just do findpath
      int findpath = 0;
      set<int> to_findpath;
      for (int i=0; i<num; i++) to_findpath.insert(find(i));

      if (args_info.verbose_lvl_arg>1) {
        fprintf(stderr, "Minima left to findpath (their father = -1): ");
        for (set<int>::iterator it=to_findpath.begin(); it!=to_findpath.end(); it++) {
          fprintf(stderr, "%d ", *it);
        }
        fprintf(stderr, "\n");
      }

      // findpath:
      for (set<int>::iterator it=to_findpath.begin(); it!=to_findpath.end(); it++) {
        set<int>::iterator it2=it;
        it2++;
        for (; it2!=to_findpath.end(); it2++) {
          energy_barr[(*it2)*num+(*it)] = energy_barr[(*it)*num+(*it2)] = find_saddle(seq, output_str[*it].c_str(), output_str[*it2].c_str(), args_info.depth_arg)/100.0;
          findpath_barr[(*it2)*num+(*it)] = findpath_barr[(*it)*num+(*it2)] = true;
          if (args_info.verbose_lvl_arg>0 && findpath %10000==0){
            fprintf(stderr, "Findpath:%7d/%7d\n", findpath, (int)(to_findpath.size()*(to_findpath.size()-1)/2));
          }
          findpath++;
        }
      }

      // debug output
      if (args_info.verbose_lvl_arg>2) {
        fprintf(stderr, "Energy barriers:\n");
        //bool symmetric = true;
        for (int i=0; i<num; i++) {
          for (int j=0; j<num; j++) {
            fprintf(stderr, "%8.2g%c ", energy_barr[i*num+j], (findpath_barr[i*num+j]?'~':' '));
            //if (energy_barr[i*num+j] != energy_barr[j*num+i]) symmetric = false;
          }
          fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
        //fprintf(stderr, "%s", (symmetric? "":"non-symmetric energy barriers!!\n"));
      }

      // time?
      if (args_info.verbose_lvl_arg>0) {
        fprintf(stderr, "Findpath(%d/%d): %.2f secs.\n", findpath, num*(num-1)/2, (clock() - clck1)/(double)CLOCKS_PER_SEC);
        clck1 = clock();
      }

      // create rates for treekin
      if (args_info.rates_flag) {
        print_rates(args_info.rates_file_arg, args_info.temp_arg, num, energy_barr, output_en);
      }

      // generate barrier tree?
      if (args_info.bartree_flag) {

        //PS_tree_plot(nodes, num, "tst.ps");

        // make tree (fill missing nodes)
        make_tree(num, energy_barr, findpath_barr, nodes);

        // plot it!
        PS_tree_plot(nodes, num, args_info.barr_name_arg);
      }

      // time?
      if (args_info.verbose_lvl_arg>0) {
        fprintf(stderr, "Rates + barrier tree generation: %.2f secs.\n", (clock() - clck1)/(double)CLOCKS_PER_SEC);
        clck1 = clock();
      }

      // printf output with fathers!
      printf("     %s\n", seq);
      for (unsigned int i=0; i<output_str.size(); i++) {
        if (args_info.eRange_given) {
          if ((output_en[i] - output_en[0]) >  args_info.eRange_arg*100 ) {
            break;
          }
        }
        printf("%4d %s %6.2f", i+1, output_str[i].c_str(), output_en[i]/100.0);
        printf(" %4d %6.2f %6d\n", nodes[i].father+1, nodes[i].saddle_height-nodes[i].height, output_num[i]);
      }

      // release smth
      for(unsigned int i=0; i<output_he.size(); i++) {
        free(output_he[i].structure);
      }
    } else {

      // printf output without fathers!
      printf("     %s\n", seq);
      for (unsigned int i=0; i<output_str.size(); i++) {
        if (args_info.eRange_given) {
          if ((output_en[i] - output_en[0]) >  args_info.eRange_arg*100 ) {
            break;
          }
        }
        printf("%4d %s %6.2f %6d", i+1, output_str[i].c_str(), output_en[i]/100.0, output_num[i]);
        printf("\n");
      }
    }

    // release res:
    if (energy_barr!=NULL) free(energy_barr);
    if (findpath_barr!=NULL) free(findpath_barr);

  } else { // fix-barrier part (just print output):
    int mfe = output_barr.begin()->first.energy;
    printf("     %s\n", seq);
    int i=1;
    for (map<hash_entry, barr_info, compare_map>::iterator it=output_barr.begin(); it!=output_barr.end(); it++) {
      if (args_info.eRange_given) {
        if ((it->first.energy - mfe) >  args_info.eRange_arg*100 ) {
          break;
        }
      }
      const hash_entry &he = it->first;
      barr_info &bi = it->second;
<<<<<<< HEAD
      printf("%4d %s %6.2f", i+1, pt_to_str(he.structure).c_str(), he.energy/100.0);
=======
      printf("%4d %s %6.2f", i, pt_to_str(he.structure).c_str(), he.energy/100.0);
>>>>>>> b426a76ce4b39eb9221db9bee82565ecbbd390f8
      printf(" %4d %6.2f %6d %6d %10.6f %6d %10.6f\n", bi.father+1, bi.e_diff/100.0, bi.bsize, bi.fbsize, bi.fen, bi.grad, bi.feng);
      i++;
    }
  }

  // release resources
  if (seq!=NULL) free(seq);
  if (name!=NULL) free(name);
  cmdline_parser_free(&args_info);

  // time?
  if (args_info.verbose_lvl_arg>0) {
    fprintf(stderr, "Printing results + freeing args: %.2f secs.\n", (clock() - clck1)/(double)CLOCKS_PER_SEC);
    clck1 = clock();
  }

  return 0;
}

char *read_previous(char *previous, map<hash_entry, int, compare_map> &output)
{
  char *seq;
  FILE *fprev;
  fprev = fopen(previous, "r");
  if (fprev == NULL) {
    fprintf(stderr, "Cannot open file \"%s\".\n", previous);
    exit(EXIT_FAILURE);
  }
  char *line = my_getline(fprev);
  char *p = strtok(line, " \t\n");
  while (p) {
    if (isSeq(p)) {
      seq = (char*)malloc((strlen(p)+1)*sizeof(char));
      strcpy(seq, p);
      break;
    }
    p = strtok(NULL, " \t\n");
  }
  free(line);

  if (seq == NULL) {
    fprintf(stderr, "Couldn't find sequence on first line of file \"%s\"\n", previous);
    fclose(fprev);
    exit(EXIT_FAILURE);
  }
  // read previously found minima:
  while ((line = my_getline(fprev))) {
    p = strtok(line, " \t\n");

    hash_entry he;
    he.structure = NULL;
    he.energy = INT_MAX;

    // read the stuff
    sscanf(p, "%d", &he.num);
    p = strtok(NULL, " \t\n");
    if (p && isStruct(p)) {
      he.structure = make_pair_table(p);
    }
    p = strtok(NULL, " \t\n");
    if (p && he.structure && he.energy==INT_MAX) {
      float en;
      if (sscanf(p, "%f", &en)==1) {
        he.energy = en_fltoi(en);
      }
    }
    // 2 aternatives: read father, e_diff and conut; or read only count
    int father;
    float e_diff;
    int count;
    p = strtok(NULL, " \t\n");
    if (p && he.structure && he.energy!=INT_MAX) {
      sscanf(p, "%d", &father);
      p = strtok(NULL, " \t\n");
      if (p && he.structure && he.energy!=INT_MAX) {
        sscanf(p, "%f", &e_diff);
        p = strtok(NULL, " \t\n");
        if (p && he.structure && he.energy!=INT_MAX) {
          sscanf(p, "%d", &count);
        }
      } else {
        count = father;
      }
    }
    // insert
    output[he]=count;
    free(line);
  }

  fclose(fprev);
  return seq;
}

char *read_barr(char *barr_arg, map<hash_entry, barr_info, compare_map> &output)
{
  char *seq = NULL;
  FILE *fbarr;
  fbarr = fopen(barr_arg, "r");
  if (fbarr == NULL) {
    fprintf(stderr, "Cannot open file \"%s\".\n", barr_arg);
    exit(EXIT_FAILURE);
  }
  char *line = my_getline(fbarr);
  char *p = strtok(line, " \t\n");
  while (p) {
    if (isSeq(p)) {
      seq = (char*)malloc((strlen(p)+1)*sizeof(char));
      strcpy(seq, p);
      break;
    }
    p = strtok(NULL, " \t\n");
  }
  free(line);

  if (seq == NULL) {
    fprintf(stderr, "Couldn't find sequence on first line of file \"%s\"\n", barr_arg);
    fclose(fbarr);
    exit(EXIT_FAILURE);
  }
  // read previously found minima:
  while ((line = my_getline(fbarr))) {
    p = strtok(line, " \t\n");

    hash_entry he;
    he.structure = NULL;
    he.energy = INT_MAX;

    // read the stuff
    sscanf(p, "%d", &he.num);
    p = strtok(NULL, " \t\n");
    if (p && isStruct(p)) {
      he.structure = make_pair_table(p);
    }
    p = strtok(NULL, " \t\n");
    if (p && he.structure && he.energy==INT_MAX) {
      float en;
      if (sscanf(p, "%f", &en)==1) {
        he.energy = en_fltoi(en);
      }
    }
    barr_info bi;
    int i =0;
    float en;
    while (p && he.structure && he.energy!=INT_MAX) {
      p = strtok(NULL, " \t\n");
      i++;
      switch (i){
        case 1: sscanf(p, "%d", &bi.father); break;
        case 2: sscanf(p, "%f", &en); bi.e_diff = en_fltoi(en); break;
        case 3: sscanf(p, "%d", &bi.bsize); break;
        case 4: sscanf(p, "%d", &bi.fbsize); break;
        case 5: sscanf(p, "%f", &bi.fen); break;
        case 6: sscanf(p, "%d", &bi.grad); break;
        case 7: sscanf(p, "%f", &bi.feng); break;
      }
    }

    // try to move it:
    Enc.Init(seq);
    he.energy = Enc.Energy(he);
    //fprintf(stderr, "%f\n", he.energy);
    int last_en = he.energy;
    while ((Opt.rand? move_rand(he) : move_set(he))!=0) {
      Deg.Clear();
    }
    Deg.Clear();

    // print changes:
    if (last_en != he.energy) {
      fprintf(stderr, "%6.2f -> %6.2f %s\n", last_en/100.0, he.energy/100.0, pt_to_str(he.structure).c_str());
    }

    // if we have it already:
    map<hash_entry, barr_info, compare_map>::iterator it;
    if ((it = output.find(he))!=output.end()) {
      it->second = it->second + bi;
      it->second.e_diff += last_en - he.energy;
      free(he.structure);
    } else {
      output[he] = bi;
    }

    free(line);
  }

  fclose(fbarr);
  return seq;
}

char *read_seq(char *seq_arg, char **name_out)
{
  char *name = NULL;
  char *seq = NULL;
  // read sequence
  FILE *fseq;
  fseq = fopen(seq_arg, "r");
  if (fseq == NULL) {
    fprintf(stderr, "Cannot open file \"%s\".\n", seq_arg);
    exit(EXIT_FAILURE);
  }
  name = my_getline(fseq);
  if (name == NULL) {
    fprintf(stderr, "File \"%s\" empty.\n", seq_arg);
    fclose(fseq);
    exit(EXIT_FAILURE);
  }
  seq = my_getline(fseq);
  if (seq == NULL || name[0]!='>') {
    //fprintf(stderr, "WARNING: File \"%s\" not in FASTA format. Using it as a sequence.\n", seq_arg);
    if (seq!=NULL) free(seq);
    seq = name;
    name = NULL;
  }
  fclose(fseq);
  if (name) (*name_out) = name;
  return seq;
}


int move(unordered_map<hash_entry, gw_struct, hash_fncts> &structs, map<hash_entry, int, compare_map> &output, set<hash_entry, compare_map> &output_shallow)
{
  // count moves
  num_moves++;

  // read a line
  char *line = my_getline(stdin);
  if (line == NULL) return -1;
  if (line[0]=='>') {
    free(line);
    return 0;
  }

  float energy=1e10;

  // process lines
  char *p = line;
  char *sep = " \t\n";
  char *temp;

  bool struct_found = false;
  bool energy_found = false;

  p = strtok(line, sep);
  while(p!=NULL && !(struct_found && energy_found)) {
    //fprintf(stderr, "%s\n", p);
    if (isStruct(p)) {
      if (struct_found) fprintf(stderr, "WARNING: On line \"%s\" two structure-like sequences found!\n", line);
      else {
        temp = p;
      }
      struct_found = true;
    } else {
      if (isEnergy(p, energy)) {
        energy_found = true;
      }
    }

    p = strtok(NULL, sep);
  }
  p = temp;
  if (!struct_found) {
    fprintf(stderr, "WARNING: On line \"%s\" no structure-like sequences found!\n", line);
    free(line);
    return 0;
  }

  // find length of structure
  int len=0;
  while (p[len]!='\0' && p[len]!=' ') len++;

  if (len!=seq_len) {
    fprintf(stderr, "Unequal lengths:\n(structure) %s\n (sequence) %s\n", p, Enc.seq);
    free(line);
    return -1;
  }

  // was it before?
  hash_entry str;
  str.structure = Enc.Struct(p);
  free(line);
  unordered_map<hash_entry, gw_struct, hash_fncts>::iterator it_s = structs.find(str);
  //hash_entry *tmp_h = (hash_entry*)lookup_hash(str);
  // if it was - release memory + get another
  if (it_s != structs.end()) {
    it_s->second.count++;
    free(str.structure);
    return 0;
  } else {
    // find energy only if not in input (not working - does energy_of_move require energy_of_struct run first???)
    str.energy = Enc.Energy(str);
    /*if (1 || !energy_found) str.energy = Enc.Energy(str);
    else str.energy = (int)(energy*100.0+(energy<0.0 ? -0.5 : 0.5));*/

    //if (en != str.energy) fprintf(stderr, "%d %d\n", en, str.energy);

    // insert into hash
    gw_struct &gw = structs[str];
    gw.count = 1;

    //is it canonical (noLP)
    if (Opt.noLP && find_lone_pair(str.structure)!=-1) {
      fprintf(stderr, "WARNING: structure \"%s\" has lone pairs, skipping...\n", pt_to_str(str.structure).c_str());
      return -2;
    }

    // copy it anew
    str.structure = allocopy(str.structure);

    //debugging
    if (Opt.verbose_lvl>2) fprintf(stderr, "processing: %d %s\n", num_moves, pt_to_str(str.structure).c_str());

    // descend
    int i;
    while ((i = (Opt.rand? move_rand(str) : move_set(str)))!=0) {
      Deg.Clear();
    }
    Deg.Clear();

    if (Opt.verbose_lvl>2) fprintf(stderr, "\n  %s %d\n", pt_to_str(str.structure).c_str(), str.energy);

    // save for output
    map<hash_entry, int, compare_map>::iterator it;
    if ((it = output.find(str)) != output.end()) {
      it->second++;
      gw.he = it->first;
      free(str.structure);
    } else {
      gw.he = str;
      str.num = output.size();
      output.insert(make_pair(str, 1));
    }
  }

  return 1;
}
